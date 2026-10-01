#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _GNU_SOURCE  // getresgid, setresgid
#include "platform/input.h"

#include "graphics/paw_frame.h"
#include "utils/error.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

atomic_uint *pending_paws = NULL;
static pid_t input_child_pid = -1;

// Group granted by a setgid install; (gid_t)-1 when not setgid or once the
// group has been dropped for good.
static gid_t privileged_gid = (gid_t)-1;

void input_privilege_init(void) {
  gid_t rgid;
  gid_t egid;
  gid_t sgid;
  if (getresgid(&rgid, &egid, &sgid) != 0 || egid == rgid) {
    return;
  }
  privileged_gid = egid;
  if (setresgid((gid_t)-1, rgid, (gid_t)-1) != 0) {
    _exit(1);
  }
}

static void privilege_raise(void) {
  if (privileged_gid != (gid_t)-1 &&
      setresgid((gid_t)-1, privileged_gid, (gid_t)-1) != 0) {
    // Not fatal: opening the devices simply fails and the cat stays idle.
    bongocat_log_warning("Could not raise input group: %s", strerror(errno));
  }
}

static void privilege_lower(void) {
  if (privileged_gid != (gid_t)-1 &&
      setresgid((gid_t)-1, getgid(), (gid_t)-1) != 0) {
    _exit(1);
  }
}

void input_privilege_drop(void) {
  if (privileged_gid == (gid_t)-1) {
    return;
  }
  gid_t gid = getgid();
  gid_t rgid;
  gid_t egid;
  gid_t sgid;
  if (setresgid(gid, gid, gid) != 0 || getresgid(&rgid, &egid, &sgid) != 0 ||
      rgid != gid || egid != gid || sgid != gid) {
    _exit(1);
  }
  privileged_gid = (gid_t)-1;
}
static int wake_fd = -1;

static void wait_child_exit(pid_t pid, int max_attempts) {
  int status;
  for (int i = 0; i < max_attempts; i++) {
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid || (result < 0 && errno == ECHILD)) {
      return;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    usleep(100000);
  }
  kill(pid, SIGKILL);
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}

typedef struct {
  uint32_t paws;
  uint32_t devices;
  int64_t monotonic_ns;
} input_message_t;
static atomic_uint local_pending;
static int helper_socket = -1;
static pid_t helper_parent;
static uint32_t device_count;
static int64_t input_timestamp_us;

void input_process_events(void) {
  input_message_t message;
  ssize_t length;
  while (wake_fd >= 0 && (length = recv(wake_fd, &message, sizeof(message),
                                        MSG_DONTWAIT)) == sizeof(message)) {
    device_count = message.devices;
    if (message.paws) {
      input_timestamp_us = message.monotonic_ns / 1000;
    }
    if ((message.paws & ~(PAW_LEFT | PAW_RIGHT)) == 0) {
      atomic_fetch_or(&local_pending, message.paws);
    }
  }
  if (wake_fd >= 0 && length == 0) {
    close(wake_fd);
    wake_fd = -1;
  }
}

uint32_t input_device_count(void) {
  return device_count;
}
int64_t input_timestamp(void) {
  return input_timestamp_us;
}

bool input_device_is_keyboard(int fd) {
  unsigned long bits[(KEY_MAX + (8 * sizeof(unsigned long))) /
                     (8 * sizeof(unsigned long))] = {0};
  if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) {
    return false;
  }
  unsigned keys[] = {KEY_A, KEY_Z, KEY_ENTER, KEY_SPACE};
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    if (!(bits[keys[i] / (8 * sizeof(unsigned long))] &
          (1UL << (keys[i] % (8 * sizeof(unsigned long)))))) {
      return false;
    }
  }
  return true;
}

int input_list_devices(void) {
  DIR *dir = opendir("/dev/input");
  if (!dir) {
    fprintf(stderr, "/dev/input: %s\n", strerror(errno));
    return 1;
  }
  unsigned keyboards = 0;
  struct dirent *entry;
  while ((entry = readdir(dir))) {
    if (strncmp(entry->d_name, "event", 5) != 0) {
      continue;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
      printf("%s: inaccessible (%s)\n", path, strerror(errno));
      continue;
    }
    char name[256] = {0};
    ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
    bool keyboard = input_device_is_keyboard(fd);
    keyboards += (unsigned int)keyboard;
    printf("%s: %s (%s)\n", path, name,
           (int)keyboard ? "keyboard" : "other input");
    close(fd);
  }
  closedir(dir);
  if (!keyboards) {
    fprintf(stderr, "No accessible keyboards; check device ACLs or input group "
                    "membership.\n");
  }
  return keyboards ? 0 : 1;
}

pid_t input_get_child_pid(void) {
  return input_child_pid;
}

int input_get_wake_fd(void) {
  return wake_fd;
}

bool input_child_is_alive(void) {
  if (input_child_pid <= 0) {
    return false;
  }
  int status;
  pid_t result = waitpid(input_child_pid, &status, WNOHANG);
  if (result == 0 || (result < 0 && errno == EINTR)) {
    return true;
  }
  if (result == input_child_pid || (result < 0 && errno == ECHILD)) {
    input_child_pid = -1;
  }
  return false;
}

// Child process signal handler - exits quietly without logging
static void child_signal_handler(int sig) {
  (void)sig;
  _exit(0);
}

// Check if a device matches any configured keyboard names
static bool device_matches_name(int fd, char **names, int num_names) {
  if (num_names <= 0) {
    return false;
  }

  char buffer[256];
  memset(buffer, 0, sizeof(buffer));
  if (ioctl(fd, EVIOCGNAME(sizeof(buffer) - 1), buffer) < 0) {
    return false;
  }
  buffer[sizeof(buffer) - 1] = '\0';

  for (int i = 0; i < num_names; i++) {
    if (strstr(buffer, names[i]) != NULL) {
      return true;
    }
  }
  return false;
}

bool input_device_selected(int fd, dev_t identity, char **paths, int num_paths,
                           char **names, int num_names) {
  if (!num_paths && !num_names) {
    return input_device_is_keyboard(fd);
  }
  for (int i = 0; i < num_paths; i++) {
    struct stat selected;
    if (paths[i] && stat(paths[i], &selected) == 0 &&
        S_ISCHR(selected.st_mode) && selected.st_rdev == identity) {
      return true;
    }
  }
  return device_matches_name(fd, names, num_names);
}

// =============================================================================
// HOTPLUG INPUT CAPTURE (runs in child process)
// =============================================================================

#define MAX_ACTIVE_DEVICES 32

typedef struct active_device {
  int fd;
  char path[256];
  dev_t identity;
} active_device_t;

static void discover_input_devices(active_device_t *active_devices,
                                   char **static_paths, int num_static,
                                   char **names, int num_names) {
  DIR *dir = opendir("/dev/input");
  if (dir) {
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
      if (strncmp(entry->d_name, "event", 5) != 0) {
        continue;
      }

      char path[256];
      int path_len =
          snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
      if (path_len < 0 || path_len >= (int)sizeof(path)) {
        bongocat_log_warning("Hotplug: device path too long, skipping '%s'",
                             entry->d_name);
        continue;
      }

      // Check if already open
      bool already_open = false;
      for (int i = 0; i < MAX_ACTIVE_DEVICES; i++) {
        if (active_devices[i].fd >= 0 &&
            strcmp(active_devices[i].path, path) == 0) {
          already_open = true;
          break;
        }
      }
      if (already_open) {
        continue;
      }

      int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
      if (fd < 0) {
        continue;
      }

      struct stat device_stat;
      if (fstat(fd, &device_stat) < 0 || !S_ISCHR(device_stat.st_mode)) {
        close(fd);
        continue;
      }
      bool duplicate = false;
      for (int d = 0; d < MAX_ACTIVE_DEVICES; d++) {
        duplicate |= active_devices[d].fd >= 0 &&
                     active_devices[d].identity == device_stat.st_rdev;
      }
      if (duplicate) {
        close(fd);
        continue;
      }
      bool match = input_device_selected(fd, device_stat.st_rdev, static_paths,
                                         num_static, names, num_names);

      if (match) {
        // Find an empty slot
        int slot = -1;
        for (int i = 0; i < MAX_ACTIVE_DEVICES; i++) {
          if (active_devices[i].fd == -1) {
            slot = i;
            break;
          }
        }

        if (slot >= 0) {
          active_devices[slot].fd = fd;
          active_devices[slot].identity = device_stat.st_rdev;
          snprintf(active_devices[slot].path, sizeof(active_devices[slot].path),
                   "%s", path);
          bongocat_log_info("Hotplug: Attached device %s (fd=%d)", path, fd);
        } else {
          bongocat_log_warning("Hotplug: Too many devices, ignoring %s", path);
          close(fd);
        }
      } else {
        close(fd);
      }
    }
    closedir(dir);
  }
}

static void read_ready_devices(active_device_t *active_devices,
                               const struct pollfd *pfds, nfds_t nfds,
                               const int *pfd_to_dev,
                               bool *initial_devices_found, uint32_t count,
                               int enable_debug) {
  struct input_event ev[64];
  // Read events from ready devices
  for (nfds_t j = 0; j < nfds; j++) {
    if (pfds[j].revents & (POLLHUP | POLLERR | POLLNVAL)) {
      int i = pfd_to_dev[j];
      close(active_devices[i].fd);
      active_devices[i].fd = -1;
      *initial_devices_found = false;
      continue;
    }
    if (pfds[j].revents & POLLIN) {
      int i = pfd_to_dev[j];
      ssize_t rd = read(active_devices[i].fd, ev, sizeof(ev));

      if (rd < 0) {
        if (errno != EAGAIN
#if EWOULDBLOCK != EAGAIN
            && errno != EWOULDBLOCK
#endif
        ) {
          bongocat_log_warning("Hotplug: Read error on %s, removing",
                               active_devices[i].path);
          close(active_devices[i].fd);
          active_devices[i].fd = -1;
          *initial_devices_found = false;
        }
        continue;
      }

      if (rd == 0) {
        bongocat_log_info("Hotplug: Device disconnected %s",
                          active_devices[i].path);
        close(active_devices[i].fd);
        active_devices[i].fd = -1;
        *initial_devices_found = false;
        continue;
      }

      size_t num_events = (size_t)rd / sizeof(struct input_event);
      unsigned paws = 0;
      for (size_t k = 0; k < num_events; k++) {
        if (ev[k].type == EV_KEY && ev[k].value == 1) {
          paws |= paw_for_keycode(ev[k].code);
          if (enable_debug) {
            // Never log which key: debug output ends up in terminals and
            // system journals, where key codes amount to a keystroke log.
            bongocat_log_debug("Key press from %s", active_devices[i].path);
          }
        }
      }

      if (paws) {
        struct timespec stamp;
        clock_gettime(CLOCK_MONOTONIC, &stamp);
        input_message_t message = {.paws = paws,
                                   .devices = count,
                                   .monotonic_ns =
                                       ((int64_t)stamp.tv_sec * 1000000000) +
                                       stamp.tv_nsec};
        if (send(helper_socket, &message, sizeof(message),
                 MSG_NOSIGNAL | MSG_DONTWAIT) < 0 &&
            errno != EAGAIN && errno != EINTR) {
          break;
        }
      }
    }
  }
}

static nfds_t prepare_device_poll(const active_device_t *active_devices,
                                  struct pollfd *pfds, int *pfd_to_dev) {
  nfds_t nfds = 0;
  for (int i = 0; i < MAX_ACTIVE_DEVICES; i++) {
    if (active_devices[i].fd >= 0) {
      pfds[nfds].fd = active_devices[i].fd;
      pfds[nfds].events = POLLIN;
      pfds[nfds].revents = 0;
      pfd_to_dev[nfds] = i;
      nfds++;
    }
  }
  return nfds;
}

static void setup_helper_signals(void) {
  // Execution resets this on setgid installations; arm it in the helper.
  if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0) {
    _exit(1);
  }

  // Check if parent already died before we set PR_SET_PDEATHSIG
  if (getppid() != helper_parent || helper_parent == 1) {
    exit(0);
  }

  // Set up child-specific signal handlers
  struct sigaction sa;
  sa.sa_handler = child_signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
}

static void capture_input_hotplug(char **static_paths, int num_static,
                                  char **names, int num_names,
                                  int scan_interval, int enable_debug) {
  pid_t parent_pid = helper_parent;
  setup_helper_signals();

  bongocat_log_debug("Starting input hotplug monitor (interval: %ds)",
                     scan_interval);

  active_device_t active_devices[MAX_ACTIVE_DEVICES];

  for (int i = 0; i < MAX_ACTIVE_DEVICES; i++) {
    active_devices[i].fd = -1;
    memset(active_devices[i].path, 0, sizeof(active_devices[i].path));
  }

  struct pollfd pfds[MAX_ACTIVE_DEVICES];
  struct timespec last_scan_time = {0, 0};
  bool initial_devices_found = false;
  bool scanning_enabled = true;
  static const int fast_retry_interval = 5;

  uint32_t reported_count = UINT32_MAX;
  while (1) {
    // Check if parent is still alive
    if (getppid() != parent_pid) {
      bongocat_log_info("Parent process died, child exiting");
      break;
    }

    // Scan for devices periodically
    // Use a fast retry interval (5s) until at least one device is found,
    // then switch to the configured scan_interval.
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    int effective_interval =
        (int)initial_devices_found ? scan_interval : fast_retry_interval;
    if (scanning_enabled &&
        now.tv_sec - last_scan_time.tv_sec >= effective_interval) {
      last_scan_time = now;
      privilege_raise();
      discover_input_devices(active_devices, static_paths, num_static, names,
                             num_names);

      if (scan_interval == 0) {
        scanning_enabled = false;
        // No rescans will ever happen, so the devices already open are all
        // this process will read: give the group up for good.
        input_privilege_drop();
      } else {
        privilege_lower();
      }

      // Check if any devices are now open
      if (!initial_devices_found) {
        for (int i = 0; i < MAX_ACTIVE_DEVICES; i++) {
          if (active_devices[i].fd >= 0) {
            initial_devices_found = true;
            break;
          }
        }
        if (!initial_devices_found) {
          bongocat_log_debug("No input devices found yet, retrying in %ds",
                             fast_retry_interval);
        }
      }
    }

    uint32_t count = 0;
    for (int d = 0; d < MAX_ACTIVE_DEVICES; d++) {
      count += active_devices[d].fd >= 0;
    }
    if (count != reported_count) {
      input_message_t status = {.devices = count};
      if (send(helper_socket, &status, sizeof(status),
               MSG_NOSIGNAL | MSG_DONTWAIT) >= 0) {
        reported_count = count;
      } else if (errno != EAGAIN && errno != EINTR) {
        break;
      }
    }
    if (scan_interval == 0 && count == 0) {
      break;
    }
    // Prepare poll
    int pfd_to_dev[MAX_ACTIVE_DEVICES];
    nfds_t nfds = prepare_device_poll(active_devices, pfds, pfd_to_dev);

    if (nfds == 0) {
      // No devices open, sleep briefly before next scan
      usleep(500000);
      continue;
    }

    int ret = poll(pfds, nfds, 1000);

    if (ret < 0) {
      if (errno != EINTR) {
        bongocat_log_error("Poll error: %s", strerror(errno));
        usleep(1000000);
      }
      continue;
    }

    if (ret == 0) {
      continue;
    }

    read_ready_devices(active_devices, pfds, nfds, pfd_to_dev,
                       &initial_devices_found, count, enable_debug);
  }

  // Clean up open device fds
  for (int i = 0; i < MAX_ACTIVE_DEVICES; i++) {
    if (active_devices[i].fd >= 0) {
      close(active_devices[i].fd);
    }
  }
  bongocat_log_info("Input monitoring stopped");
}

// =============================================================================
// PUBLIC API
// =============================================================================

static bool helper_integer(const char *text, int maximum, int *value) {
  errno = 0;
  char *end;
  long parsed = strtol(text, &end, 10);
  if (errno || end == text || *end || parsed < 0 || parsed > maximum) {
    return false;
  }
  *value = (int)parsed;
  return true;
}

int input_helper_main(int argc, char **argv) {
  if (argc < 6) {
    return 1;
  }
  bongocat_error_init(0);
  helper_socket = 3;
  struct ucred peer;
  socklen_t length = sizeof(peer);
  if (getsockopt(helper_socket, SOL_SOCKET, SO_PEERCRED, &peer, &length) < 0 ||
      peer.uid != getuid() || peer.pid != getppid()) {
    return 1;
  }
  helper_parent = peer.pid;
  int num_paths;
  int num_names;
  int interval;
  if (strcmp(argv[2], "3") != 0 || !helper_integer(argv[3], 256, &num_paths) ||
      !helper_integer(argv[4], 256, &num_names) ||
      !helper_integer(argv[5], 3600, &interval)) {
    return 1;
  }
  if (num_paths < 0 || num_names < 0 || interval < 0 || interval > 3600 ||
      num_paths > 256 || num_names > 256 || argc != 6 + num_paths + num_names) {
    return 1;
  }
  capture_input_hotplug(argv + 6, num_paths, argv + 6 + num_paths, num_names,
                        interval, 0);
  return 0;
}

bongocat_error_t input_start_monitoring(char **paths, int num_paths,
                                        char **names, int num_names,
                                        int interval, int debug) {
  (void)debug;
  if (num_paths < 0 || num_names < 0 || num_paths > 256 || num_names > 256) {
    return BONGOCAT_ERROR_INVALID_PARAM;
  }
  int sockets[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0,
                 sockets) < 0) {
    return BONGOCAT_ERROR_FILE_IO;
  }
  char paths_count[16];
  char names_count[16];
  char scan[16];
  snprintf(paths_count, sizeof(paths_count), "%d", num_paths);
  snprintf(names_count, sizeof(names_count), "%d", num_names);
  snprintf(scan, sizeof(scan), "%d", interval);
  char **args =
      (char **)calloc((size_t)num_paths + num_names + 7, sizeof(*args));
  if (!args) {
    close(sockets[0]);
    close(sockets[1]);
    return BONGOCAT_ERROR_MEMORY;
  }
  args[0] = "/proc/self/exe";
  args[1] = "--input-helper";
  args[2] = "3";
  args[3] = paths_count;
  args[4] = names_count;
  args[5] = scan;
  for (int i = 0; i < num_paths; i++) {
    args[6 + i] = paths[i];
  }
  for (int i = 0; i < num_names; i++) {
    args[6 + num_paths + i] = names[i];
  }
  posix_spawn_file_actions_t actions;
  int error = posix_spawn_file_actions_init(&actions);
  if (!error) {
    error = posix_spawn_file_actions_adddup2(&actions, sockets[1], 3);
    if (!error) {
      error = posix_spawn_file_actions_addclosefrom_np(&actions, 4);
    }
    if (!error) {
      error = posix_spawn(&input_child_pid, "/proc/self/exe", &actions, NULL,
                          args, environ);
    }
    posix_spawn_file_actions_destroy(&actions);
  }
  free((void *)args);
  close(sockets[1]);
  if (error) {
    close(sockets[0]);
    input_child_pid = -1;
    return BONGOCAT_ERROR_THREAD;
  }
  wake_fd = sockets[0];
  atomic_init(&local_pending, 0);
  pending_paws = &local_pending;
  return BONGOCAT_SUCCESS;
}

bongocat_error_t input_restart_monitoring(char **paths, int num_paths,
                                          char **names, int num_names,
                                          int interval, int debug) {
  input_cleanup();
  return input_start_monitoring(paths, num_paths, names, num_names, interval,
                                debug);
}

void input_cleanup(void) {
  if (input_child_pid > 0) {
    kill(input_child_pid, SIGTERM);
    wait_child_exit(input_child_pid, 10);
    input_child_pid = -1;
  }
  if (wake_fd >= 0) {
    close(wake_fd);
  }
  wake_fd = -1;
  pending_paws = NULL;
  device_count = 0;
  input_timestamp_us = 0;
}
