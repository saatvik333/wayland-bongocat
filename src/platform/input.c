#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "platform/input.h"

#include "graphics/animation.h"
#include "utils/memory.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

atomic_int *any_key_pressed;
atomic_int *last_key_code;
static pid_t input_child_pid = -1;

// Child process signal handler - exits quietly without logging
static void child_signal_handler(int sig) {
  (void)sig;  // Suppress unused parameter warning
  exit(0);
}

// Check if a device matches any of the configured criteria
static bool device_matches_criteria(int fd, char **names, int num_names) {
  char buffer[256];

  // Check Device Name
  if (num_names > 0) {
    memset(buffer, 0, sizeof(buffer));
    if (ioctl(fd, EVIOCGNAME(sizeof(buffer) - 1), buffer) >= 0) {
      buffer[sizeof(buffer) - 1] = '\0';
      for (int i = 0; i < num_names; i++) {
        if (strstr(buffer, names[i]) != NULL) {
          return true;
        }
      }
    }
  }

  return false;
}

static void capture_input_hotplug(char **static_paths, int num_static,
                                  char **names, int num_names,
                                  int scan_interval, int enable_debug) {
  // CRITICAL: Set this process to die when parent dies
  // This prevents ghost child processes if parent crashes
  prctl(PR_SET_PDEATHSIG, SIGTERM);

  // Check if parent already died before we set PR_SET_PDEATHSIG
  if (getppid() == 1) {
    exit(0);  // Parent already died, orphaned to init
  }

  // Set up child-specific signal handlers to avoid duplicate logging
  struct sigaction sa;
  sa.sa_handler = child_signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);

  bongocat_log_debug("Starting input hotplug monitor (interval: %ds)",
                     scan_interval);

// Track up to 32 active devices
#define MAX_ACTIVE_DEVICES 32
  struct {
    int fd;
    char path[256];
  } active_devices[MAX_ACTIVE_DEVICES];

  for (int i = 0; i < MAX_ACTIVE_DEVICES; i++) {
    active_devices[i].fd = -1;
    memset(active_devices[i].path, 0, sizeof(active_devices[i].path));
  }

  struct input_event ev[64];
  fd_set readfds;
  struct timeval timeout;
  struct timespec last_scan_time = {
      0, 0};  // Initialize to 0 to force immediate first scan

  while (1) {
    // 1. Scan for new devices periodically (time-based)
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    // Only scan if interval > 0. If 0, scan only once at start (handled by
    // last_scan_time=0) If interval is huge (e.g. 600s), it scans every 10
    // mins.
    if (now.tv_sec - last_scan_time.tv_sec >= scan_interval) {
      last_scan_time = now;
      DIR *dir = opendir("/dev/input");
      if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
          if (strncmp(entry->d_name, "event", 5) != 0)
            continue;

          char path[256];
          snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);

          // Check if already open
          bool already_open = false;
          for (int i = 0; i < MAX_ACTIVE_DEVICES; i++) {
            if (active_devices[i].fd >= 0 &&
                strcmp(active_devices[i].path, path) == 0) {
              already_open = true;
              break;
            }
          }
          if (already_open)
            continue;

          // Try to open and check criteria
          int fd = open(path, O_RDONLY | O_NONBLOCK);
          if (fd < 0)
            continue;

          bool match = false;

          // Check static paths
          for (int i = 0; i < num_static; i++) {
            if (strcmp(path, static_paths[i]) == 0) {
              match = true;
              break;
            }
          }

          // Check dynamic criteria
          if (!match) {
            match = device_matches_criteria(fd, names, num_names);
          }

          if (match) {
            // Find slot
            int slot = -1;
            for (int i = 0; i < MAX_ACTIVE_DEVICES; i++) {
              if (active_devices[i].fd == -1) {
                slot = i;
                break;
              }
            }

            if (slot >= 0) {
              active_devices[slot].fd = fd;
              strncpy(active_devices[slot].path, path,
                      sizeof(active_devices[slot].path) - 1);
              bongocat_log_info("Hotplug: Attached device %s (fd=%d)", path,
                                fd);
            } else {
              bongocat_log_warning("Hotplug: Too many devices, ignoring %s",
                                   path);
              close(fd);
            }
          } else {
            close(fd);
          }
        }
        closedir(dir);
      }
    }

    // 2. Prepare select
    FD_ZERO(&readfds);
    int max_fd = -1;
    int count = 0;
    for (int i = 0; i < MAX_ACTIVE_DEVICES; i++) {
      if (active_devices[i].fd >= 0) {
        FD_SET(active_devices[i].fd, &readfds);
        if (active_devices[i].fd > max_fd)
          max_fd = active_devices[i].fd;
        count++;
      }
    }

    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    int ret = select(max_fd + 1, &readfds, NULL, NULL, &timeout);

    if (getppid() == 1)
      break;  // Check parent again

    if (ret < 0) {
      if (errno != EINTR) {
        bongocat_log_error("Select error: %s", strerror(errno));
        usleep(1000000);  // Sleep 1s on error to prevent busy loop
      }
      continue;
    }

    if (ret == 0)
      continue;  // Timeout

    // 3. Read events
    for (int i = 0; i < MAX_ACTIVE_DEVICES; i++) {
      if (active_devices[i].fd >= 0 &&
          FD_ISSET(active_devices[i].fd, &readfds)) {
        int rd = read(active_devices[i].fd, ev, sizeof(ev));

        if (rd < 0) {
          if (errno != EAGAIN && errno != EWOULDBLOCK) {
            bongocat_log_warning("Hotplug: Read error on %s, removing",
                                 active_devices[i].path);
            close(active_devices[i].fd);
            active_devices[i].fd = -1;
          }
          continue;
        }

        if (rd == 0) {
          bongocat_log_info("Hotplug: Device disconnected %s",
                            active_devices[i].path);
          close(active_devices[i].fd);
          active_devices[i].fd = -1;
          continue;
        }

        int num_events = rd / sizeof(struct input_event);
        bool key_pressed = false;
        int code = 0;

        for (int j = 0; j < num_events; j++) {
          if (ev[j].type == EV_KEY && ev[j].value == 1) {
            key_pressed = true;
            code = ev[j].code;
            if (enable_debug) {
              bongocat_log_debug("Key: %d from %s", code,
                                 active_devices[i].path);
            }
          }
        }

        if (key_pressed) {
          atomic_store(last_key_code, code);
          animation_trigger();
        }
      }
    }
  }
}

bongocat_error_t input_start_monitoring(char **device_paths, int num_devices,
                                        char **names, int num_names,
                                        int scan_interval, int enable_debug) {
  bongocat_log_info("Initializing input hotplug system");

  // Initialize shared memory
  any_key_pressed =
      (atomic_int *)mmap(NULL, sizeof(atomic_int), PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (any_key_pressed == MAP_FAILED)
    return BONGOCAT_ERROR_MEMORY;
  atomic_store(any_key_pressed, 0);

  last_key_code =
      (atomic_int *)mmap(NULL, sizeof(atomic_int), PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (last_key_code == MAP_FAILED) {
    munmap(any_key_pressed, sizeof(atomic_int));
    return BONGOCAT_ERROR_MEMORY;
  }
  atomic_store(last_key_code, 0);

  input_child_pid = fork();
  if (input_child_pid < 0) {
    munmap(any_key_pressed, sizeof(int));
    munmap(last_key_code, sizeof(int));
    return BONGOCAT_ERROR_THREAD;
  }

  if (input_child_pid == 0) {
    capture_input_hotplug(device_paths, num_devices, names, num_names,
                          scan_interval, enable_debug);
    exit(0);
  }

  bongocat_log_info("Input monitoring started (PID: %d)", input_child_pid);
  return BONGOCAT_SUCCESS;
}

bongocat_error_t input_restart_monitoring(char **device_paths, int num_devices,
                                          char **names, int num_names,
                                          int scan_interval, int enable_debug) {
  input_cleanup();
  return input_start_monitoring(device_paths, num_devices, names, num_names,
                                scan_interval, enable_debug);
}

void input_cleanup(void) {
  if (input_child_pid > 0) {
    kill(input_child_pid, SIGTERM);
    waitpid(input_child_pid, NULL, 0);
    input_child_pid = -1;
  }
  if (any_key_pressed && any_key_pressed != MAP_FAILED) {
    munmap(any_key_pressed, sizeof(int));
    any_key_pressed = NULL;
  }
  if (last_key_code && last_key_code != MAP_FAILED) {
    munmap(last_key_code, sizeof(int));
    last_key_code = NULL;
  }
}
