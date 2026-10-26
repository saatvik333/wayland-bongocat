#define _GNU_SOURCE
#include "core/control.h"

#include "utils/clock.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define CLIENT_TIMEOUT_US 1000000

static int lock_fd = -1;
static int server = -1;
static char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static struct {
  int fd;
  int64_t deadline;
} clients[CONTROL_MAX_CLIENTS];

// $XDG_RUNTIME_DIR/bongocat.SUFFIX, refusing a runtime directory that other
// users could write; /tmp/bongocat-UID.SUFFIX without one.
static int runtime_path(char *buffer, size_t size, const char *suffix) {
  const char *runtime = getenv("XDG_RUNTIME_DIR");
  int length;
  if (runtime && runtime[0]) {
    struct stat st;
    if (stat(runtime, &st) < 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != getuid() || (st.st_mode & 0022)) {
      errno = EPERM;
      return -1;
    }
    length = snprintf(buffer, size, "%s/bongocat.%s", runtime, suffix);
  } else {
    length = snprintf(buffer, size, "/tmp/bongocat-%lu.%s",
                      (unsigned long)getuid(), suffix);
  }
  if (length < 0 || (size_t)length >= size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  return 0;
}

int instance_lock(void) {
  char filename[PATH_MAX];
  if (runtime_path(filename, sizeof(filename), "pid") < 0) {
    return -1;
  }
  int fd = open(filename, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) {
    return -1;
  }
  struct stat st;
  if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
      st.st_nlink != 1 || (st.st_mode & 0077)) {
    close(fd);
    errno = EPERM;
    return -1;
  }
  if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
    int saved = errno;
    close(fd);
    errno = saved;
    return saved == EWOULDBLOCK ? INSTANCE_LOCKED : -1;
  }
  char pid[32];
  int length = snprintf(pid, sizeof(pid), "%ld\n", (long)getpid());
  if (ftruncate(fd, 0) < 0 || pwrite(fd, pid, (size_t)length, 0) != length) {
    close(fd);
    return -1;
  }
  lock_fd = fd;
  return 0;
}

void instance_unlock(void) {
  if (lock_fd >= 0) {
    // Keep the inode: truncating avoids an unlink race with a new instance.
    if (ftruncate(lock_fd, 0) < 0) {
      // Nothing to do; the lock is released on close regardless.
    }
    close(lock_fd);
  }
  lock_fd = -1;
}

static bool same_user(int fd) {
  struct ucred peer;
  socklen_t size = sizeof(peer);
  return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0 &&
         peer.uid == getuid();
}

int control_start(void) {
  if (lock_fd < 0 ||
      runtime_path(socket_path, sizeof(socket_path), "sock") < 0) {
    return -1;
  }
  // The lock guarantees a stale socket belongs to a dead instance.
  struct stat st;
  if (lstat(socket_path, &st) == 0) {
    if (!S_ISSOCK(st.st_mode) || st.st_uid != getuid()) {
      errno = EPERM;
      return -1;
    }
    if (unlink(socket_path) < 0) {
      return -1;
    }
  } else if (errno != ENOENT) {
    return -1;
  }
  server = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (server < 0) {
    return -1;
  }
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  memcpy(address.sun_path, socket_path, strlen(socket_path) + 1);
  mode_t old = umask(0077);
  int result = bind(server, (struct sockaddr *)&address, sizeof(address));
  umask(old);
  if (result < 0 || listen(server, CONTROL_MAX_CLIENTS) < 0) {
    close(server);
    server = -1;
    return -1;
  }
  for (size_t i = 0; i < CONTROL_MAX_CLIENTS; i++) {
    clients[i].fd = -1;
  }
  return 0;
}

static void accept_clients(void) {
  for (;;) {
    int fd = accept4(server, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) {
      return;
    }
    bool stored = false;
    for (size_t i = 0; !stored && same_user(fd) && i < CONTROL_MAX_CLIENTS;
         i++) {
      if (clients[i].fd < 0) {
        clients[i].fd = fd;
        clients[i].deadline = monotonic_us() + CLIENT_TIMEOUT_US;
        stored = true;
      }
    }
    if (!stored) {
      close(fd);
    }
  }
}

void control_process(int (*handler)(const char *request, char *reply,
                                    size_t size)) {
  if (server < 0) {
    return;
  }
  accept_clients();
  for (size_t i = 0; i < CONTROL_MAX_CLIENTS; i++) {
    int fd = clients[i].fd;
    if (fd < 0) {
      continue;
    }
    char request[64];
    ssize_t length =
        recv(fd, request, sizeof(request), MSG_DONTWAIT | MSG_TRUNC);
    if (length < 0 && (errno == EAGAIN || errno == EINTR) &&
        monotonic_us() < clients[i].deadline) {
      continue;
    }
    // Requests are short NUL-free strings; anything else is dropped.
    if (length > 0 && (size_t)length < sizeof(request) &&
        !memchr(request, '\0', (size_t)length)) {
      request[length] = '\0';
      char detail[480];
      int status = handler(request, detail, sizeof(detail));
      char response[512];
      snprintf(response, sizeof(response), "%d %s", status, detail);
      if (send(fd, response, strlen(response), MSG_NOSIGNAL) < 0) {
        // The client gave up; nothing to report.
      }
    }
    close(fd);
    clients[i].fd = -1;
  }
}

size_t control_fds(int *fds, size_t capacity) {
  size_t count = 0;
  if (server >= 0 && count < capacity) {
    fds[count++] = server;
  }
  for (size_t i = 0; i < CONTROL_MAX_CLIENTS && count < capacity; i++) {
    if (clients[i].fd >= 0) {
      fds[count++] = clients[i].fd;
    }
  }
  return count;
}

int64_t control_deadline(void) {
  int64_t deadline = 0;
  for (size_t i = 0; server >= 0 && i < CONTROL_MAX_CLIENTS; i++) {
    if (clients[i].fd >= 0 && (!deadline || clients[i].deadline < deadline)) {
      deadline = clients[i].deadline;
    }
  }
  return deadline;
}

void control_cleanup(void) {
  if (server >= 0) {
    for (size_t i = 0; i < CONTROL_MAX_CLIENTS; i++) {
      if (clients[i].fd >= 0) {
        close(clients[i].fd);
      }
    }
    close(server);
    unlink(socket_path);
  }
  server = -1;
}

int control_request(const char *request) {
  char filename[sizeof(socket_path)];
  if (runtime_path(filename, sizeof(filename), "sock") < 0) {
    return 1;
  }
  // Nonblocking: a stuck instance must not hang the client in connect().
  int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) {
    return 1;
  }
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  memcpy(address.sun_path, filename, strlen(filename) + 1);
  if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    int saved = errno;
    close(fd);
    return saved == ENOENT || saved == ECONNREFUSED ? 2 : 1;
  }
  if (!same_user(fd) || send(fd, request, strlen(request), MSG_NOSIGNAL) < 0) {
    close(fd);
    return 1;
  }
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  char response[512];
  ssize_t length = poll(&pfd, 1, 1500) > 0
                       ? recv(fd, response, sizeof(response) - 1, 0)
                       : -1;
  close(fd);
  if (length <= 2) {
    return 1;
  }
  response[length] = '\0';
  printf("%s\n", response + 2);
  return response[0] == '0' && response[1] == ' ' ? 0 : 1;
}
