#define _GNU_SOURCE
#include "core/control.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int lock_fd = -1, server = -1;
static char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static struct {
  int fd;
  int64_t deadline;
} clients[16];
static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((int64_t)ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}
static int path(char *buffer, size_t size, const char *suffix) {
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
    {
      length = snprintf(buffer, size, "/tmp/bongocat-%lu.%s",
                        (unsigned long)getuid(), suffix);
    }
  }
  if (length < 0 || (size_t)length >= size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  return 0;
}
int instance_lock(void) {
  char filename[PATH_MAX];
  if (path(filename, sizeof(filename), "pid") < 0) {
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
    return saved == EWOULDBLOCK ? -2 : -1;
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
    int result = ftruncate(lock_fd, 0);
    (void)result;
    close(lock_fd);
  }
  lock_fd = -1;
}
static bool authenticated(int fd) {
  struct ucred peer;
  socklen_t size = sizeof(peer);
  return (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0 &&
          peer.uid == getuid()) != 0;
}
int control_start(void) {
  if (lock_fd < 0 || path(socket_path, sizeof(socket_path), "sock") < 0) {
    return -1;
  }
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
    { return -1; }
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
  if (result < 0 || listen(server, 16) < 0) {
    close(server);
    server = -1;
    return -1;
  }
  for (size_t i = 0; i < 16; i++) {
    clients[i].fd = -1;
  }
  return 0;
}
void control_process(int (*handler)(const char *, char *, size_t)) {
  if (server < 0) {
    return;
  }
  for (;;) {
    int fd = accept4(server, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) {
      break;
    }
    bool stored = false;
    if (authenticated(fd)) {
      for (size_t i = 0; i < 16; i++) {
        if (clients[i].fd < 0) {
          clients[i].fd = fd;
          clients[i].deadline = now_ms() + 1000;
          stored = true;
          break;
        }
      }
    }
    if (!stored) {
      close(fd);
    }
  }
  for (size_t i = 0; i < 16; i++) {
    int fd = clients[i].fd;
    if (fd < 0) {
      continue;
    }
    char request[64];
    char response[512];
    ssize_t length =
        recv(fd, request, sizeof(request), MSG_DONTWAIT | MSG_TRUNC);
    if (length < 0 && (errno == EAGAIN || errno == EINTR) &&
        now_ms() < clients[i].deadline) {
      continue;
    }
    if (length > 0 && (size_t)length < sizeof(request) &&
        !memchr(request, '\0', (size_t)length)) {
      request[length] = '\0';
      char detail[480];
      int status = handler(request, detail, sizeof(detail));
      snprintf(response, sizeof(response), "%d %s", status, detail);
      ssize_t sent = send(fd, response, strlen(response), MSG_NOSIGNAL);
      (void)sent;
    }
    close(fd);
    clients[i].fd = -1;
  }
}
int control_fds(int *fds, size_t capacity) {
  size_t count = 0;
  if (server >= 0 && count < capacity) {
    fds[count++] = server;
  }
  for (size_t i = 0; i < 16 && count < capacity; i++) {
    if (clients[i].fd >= 0) {
      fds[count++] = clients[i].fd;
    }
  }
  return (int)count;
}
void control_cleanup(void) {
  if (server >= 0) {
    for (size_t i = 0; i < 16; i++) {
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
  if (path(filename, sizeof(filename), "sock") < 0) {
    return 1;
  }
  int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) {
    return 1;
  }
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  memcpy(address.sun_path, filename, strlen(filename) + 1);
  if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    int saved = errno;
    close(fd);
    if (saved == ENOENT || saved == ECONNREFUSED) {
      return 2;
    }
    return 1;
  }
  if (!authenticated(fd) ||
      send(fd, request, strlen(request), MSG_NOSIGNAL) < 0) {
    close(fd);
    return 1;
  }
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  int result = poll(&pfd, 1, 1500);
  char response[512];
  ssize_t length =
      result > 0 ? recv(fd, response, sizeof(response) - 1, 0) : -1;
  close(fd);
  if (length <= 2 || (size_t)length >= sizeof(response)) {
    return 1;
  }
  response[length] = '\0';
  printf("%s\n", response + 2);
  return response[0] == '0' && response[1] == ' ' ? 0 : 1;
}

int control_timeout(void) {
  int timeout = -1;
  if (server < 0) {
    return timeout;
  }
  for (size_t i = 0; i < 16; i++) {
    if (clients[i].fd < 0) {
      continue;
    }
    int64_t remaining = clients[i].deadline - now_ms();
    int delay = remaining > 0 ? (int)remaining : 0;
    if (timeout < 0 || delay < timeout) {
      timeout = delay;
    }
  }
  return timeout;
}
