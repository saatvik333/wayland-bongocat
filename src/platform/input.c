#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "platform/input.h"

#include "graphics/paw_frame.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

atomic_uint *pending_paws = NULL;
static pid_t input_child_pid = -1;
static int wake_fd = -1;

static void wait_child_exit(pid_t pid, int max_attempts) {
  int status;
  for (int i = 0; i < max_attempts; i++) {
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid || result == -1)
      return;
    usleep(100000);
  }
  kill(pid, SIGKILL);
  waitpid(pid, &status, 0);
}

static atomic_uint *alloc_shared_pending_paws(void) {
  atomic_uint *ptr = mmap(NULL, sizeof(*ptr), PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (ptr == MAP_FAILED)
    return NULL;
  atomic_init(ptr, 0u);
  return ptr;
}

pid_t input_get_child_pid(void) {
  return input_child_pid;
}

int input_get_wake_fd(void) {
  return wake_fd;
}

bool input_child_is_alive(void) {
  if (input_child_pid <= 0)
    return false;
  int status;
  pid_t result = waitpid(input_child_pid, &status, WNOHANG);
  if (result == 0 || (result < 0 && errno == EINTR))
    return true;
  if (result == input_child_pid || (result < 0 && errno == ECHILD))
    input_child_pid = -1;
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

// =============================================================================
// HOTPLUG INPUT CAPTURE (runs in child process)
// =============================================================================

#define MAX_ACTIVE_DEVICES 32

static void capture_input_hotplug(char **static_paths, int num_static,
                                  char **names, int num_names,
                                  int scan_interval, int enable_debug) {
  // CRITICAL: Set this process to die when parent dies
  prctl(PR_SET_PDEATHSIG, SIGTERM);

  // Check if parent already died before we set PR_SET_PDEATHSIG
  if (getppid() == 1) {
    exit(0);
  }

  // Set up child-specific signal handlers
  struct sigaction sa;
  sa.sa_handler = child_signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);

  bongocat_log_debug("Starting input hotplug monitor (interval: %ds)",
                     scan_interval);

  struct {
    int fd;
    char path[256];
  } active_devices[MAX_ACTIVE_DEVICES];

  for (int i = 0; i < MAX_ACTIVE_DEVICES; i++) {
    active_devices[i].fd = -1;
    memset(active_devices[i].path, 0, sizeof(active_devices[i].path));
  }

  struct input_event ev[64];
  struct pollfd pfds[MAX_ACTIVE_DEVICES];
  struct timespec last_scan_time = {0, 0};
  bool initial_devices_found = false;
  bool scanning_enabled = true;
  static const int FAST_RETRY_INTERVAL = 5;

  while (1) {
    // Check if parent is still alive
    if (getppid() == 1) {
      bongocat_log_info("Parent process died, child exiting");
      break;
    }

    // Scan for devices periodically
    // Use a fast retry interval (5s) until at least one device is found,
    // then switch to the configured scan_interval.
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    int effective_interval =
        initial_devices_found ? scan_interval : FAST_RETRY_INTERVAL;
    if (scanning_enabled &&
        now.tv_sec - last_scan_time.tv_sec >= effective_interval) {
      last_scan_time = now;
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

          bool match = false;

          // Check static device paths
          for (int i = 0; i < num_static; i++) {
            if (static_paths[i] && strcmp(path, static_paths[i]) == 0) {
              match = true;
              break;
            }
          }

          // Check device name matching
          if (!match) {
            match = device_matches_name(fd, names, num_names);
          }

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
              snprintf(active_devices[slot].path,
                       sizeof(active_devices[slot].path), "%s", path);
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

      if (scan_interval == 0)
        scanning_enabled = false;

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
                             FAST_RETRY_INTERVAL);
        }
      }
    }

    // Prepare poll
    nfds_t nfds = 0;
    int pfd_to_dev[MAX_ACTIVE_DEVICES];
    for (int i = 0; i < MAX_ACTIVE_DEVICES; i++) {
      if (active_devices[i].fd >= 0) {
        pfds[nfds].fd = active_devices[i].fd;
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        pfd_to_dev[nfds] = i;
        nfds++;
      }
    }

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

    // Read events from ready devices
    for (nfds_t j = 0; j < nfds; j++) {
      if (pfds[j].revents & POLLIN) {
        int i = pfd_to_dev[j];
        int rd = read(active_devices[i].fd, ev, sizeof(ev));

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
        for (int k = 0; k < num_events; k++) {
          if (ev[k].type == EV_KEY && ev[k].value == 1) {
            key_pressed = true;
            if (pending_paws) {
              atomic_fetch_or_explicit(pending_paws,
                                       paw_for_keycode(ev[k].code),
                                       memory_order_release);
            }
            if (enable_debug) {
              bongocat_log_debug("Key: %d from %s", ev[k].code,
                                 active_devices[i].path);
            }
          }
        }

        if (key_pressed) {
          if (wake_fd >= 0) {
            uint64_t val = 1;
            if (write(wake_fd, &val, sizeof(val)) < 0) {
              // Best-effort wake; ignore errors
            }
          }
        }
      }
    }
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

bongocat_error_t input_start_monitoring(char **device_paths, int num_devices,
                                        char **names, int num_names,
                                        int scan_interval, int enable_debug) {
  bongocat_log_info("Initializing input hotplug system");

  pending_paws = alloc_shared_pending_paws();
  if (!pending_paws) {
    bongocat_log_error("Failed to create shared pending-paw state: %s",
                       strerror(errno));
    return BONGOCAT_ERROR_MEMORY;
  }

  wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wake_fd < 0) {
    bongocat_log_warning(
        "Failed to create eventfd: %s (falling back to polling)",
        strerror(errno));
  }

  input_child_pid = fork();
  if (input_child_pid < 0) {
    bongocat_log_error("Failed to fork input monitoring process: %s",
                       strerror(errno));
    munmap(pending_paws, sizeof(*pending_paws));
    pending_paws = NULL;
    if (wake_fd >= 0) {
      close(wake_fd);
      wake_fd = -1;
    }
    return BONGOCAT_ERROR_THREAD;
  }

  if (input_child_pid == 0) {
    capture_input_hotplug(device_paths, num_devices, names, num_names,
                          scan_interval, enable_debug);
    exit(0);
  }

  bongocat_log_info("Input monitoring started (child PID: %d)",
                    input_child_pid);
  return BONGOCAT_SUCCESS;
}

bongocat_error_t input_restart_monitoring(char **device_paths, int num_devices,
                                          char **names, int num_names,
                                          int scan_interval, int enable_debug) {
  bongocat_log_info("Restarting input monitoring system");

  // Stop current monitoring
  if (input_child_pid > 0) {
    bongocat_log_debug("Stopping current input monitoring (PID: %d)",
                       input_child_pid);
    kill(input_child_pid, SIGTERM);
    wait_child_exit(input_child_pid, 10);
    input_child_pid = -1;
  }

  bool need_new_state = (pending_paws == NULL || pending_paws == MAP_FAILED);
  if (need_new_state) {
    pending_paws = alloc_shared_pending_paws();
    if (!pending_paws) {
      bongocat_log_error("Failed to create shared pending-paw state: %s",
                         strerror(errno));
      return BONGOCAT_ERROR_MEMORY;
    }
  }

  atomic_store(pending_paws, 0u);

  // Recreate eventfd for the new child process
  if (wake_fd >= 0) {
    close(wake_fd);
  }
  wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wake_fd < 0) {
    bongocat_log_warning("Failed to recreate eventfd: %s", strerror(errno));
  }

  // Fork new process
  input_child_pid = fork();
  if (input_child_pid < 0) {
    bongocat_log_error("Failed to fork input monitoring process: %s",
                       strerror(errno));
    if (need_new_state) {
      munmap(pending_paws, sizeof(*pending_paws));
      pending_paws = NULL;
    }
    return BONGOCAT_ERROR_THREAD;
  }

  if (input_child_pid == 0) {
    capture_input_hotplug(device_paths, num_devices, names, num_names,
                          scan_interval, enable_debug);
    exit(0);
  }

  bongocat_log_info("Input monitoring restarted (child PID: %d)",
                    input_child_pid);
  return BONGOCAT_SUCCESS;
}

void input_cleanup(void) {
  bongocat_log_info("Cleaning up input monitoring system");

  if (input_child_pid > 0) {
    bongocat_log_debug("Terminating input monitoring child process (PID: %d)",
                       input_child_pid);
    kill(input_child_pid, SIGTERM);
    wait_child_exit(input_child_pid, 10);
    input_child_pid = -1;
  }

  // Cleanup eventfd
  if (wake_fd >= 0) {
    close(wake_fd);
    wake_fd = -1;
  }

  // Cleanup shared memory
  if (pending_paws && pending_paws != MAP_FAILED) {
    munmap(pending_paws, sizeof(*pending_paws));
    pending_paws = NULL;
  }

  bongocat_log_debug("Input monitoring cleanup complete");
}
