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

pid_t input_get_child_pid(void) {
  return input_child_pid;
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
  fd_set readfds;
  struct timeval timeout;
  struct timespec last_scan_time = {0, 0};

  while (1) {
    // Check if parent is still alive
    if (getppid() == 1) {
      bongocat_log_info("Parent process died, child exiting");
      break;
    }

    // Scan for devices periodically
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (now.tv_sec - last_scan_time.tv_sec >= scan_interval) {
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

          int fd = open(path, O_RDONLY | O_NONBLOCK);
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

      // If scan_interval is 0, only scan once (at startup) and never again
      if (scan_interval == 0) {
        // Set to INT_MAX so the condition never triggers again
        last_scan_time.tv_sec = now.tv_sec;
        scan_interval = __INT_MAX__;
      }
    }

    // Prepare select
    FD_ZERO(&readfds);
    int max_fd = -1;
    for (int i = 0; i < MAX_ACTIVE_DEVICES; i++) {
      if (active_devices[i].fd >= 0) {
        if (active_devices[i].fd >= FD_SETSIZE) {
          bongocat_log_error("fd %d exceeds FD_SETSIZE (%d), skipping",
                             active_devices[i].fd, FD_SETSIZE);
          continue;
        }
        FD_SET(active_devices[i].fd, &readfds);
        if (active_devices[i].fd > max_fd) {
          max_fd = active_devices[i].fd;
        }
      }
    }

    if (max_fd < 0) {
      // No devices open, sleep briefly before next scan
      usleep(500000);
      continue;
    }

    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    int ret = select(max_fd + 1, &readfds, NULL, NULL, &timeout);

    if (ret < 0) {
      if (errno != EINTR) {
        bongocat_log_error("Select error: %s", strerror(errno));
        usleep(1000000);
      }
      continue;
    }

    if (ret == 0) {
      continue;
    }

    // Read events from ready devices
    for (int i = 0; i < MAX_ACTIVE_DEVICES; i++) {
      if (active_devices[i].fd >= 0 &&
          FD_ISSET(active_devices[i].fd, &readfds)) {
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

  // Initialize shared memory for key press state
  any_key_pressed =
      (atomic_int *)mmap(NULL, sizeof(atomic_int), PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (any_key_pressed == MAP_FAILED) {
    bongocat_log_error("Failed to create shared memory for input: %s",
                       strerror(errno));
    return BONGOCAT_ERROR_MEMORY;
  }
  atomic_store(any_key_pressed, 0);

  // Shared memory for last key code (hand mapping)
  last_key_code =
      (atomic_int *)mmap(NULL, sizeof(atomic_int), PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (last_key_code == MAP_FAILED) {
    bongocat_log_error("Failed to create shared memory for key code: %s",
                       strerror(errno));
    munmap(any_key_pressed, sizeof(atomic_int));
    return BONGOCAT_ERROR_MEMORY;
  }
  atomic_store(last_key_code, 0);

  input_child_pid = fork();
  if (input_child_pid < 0) {
    bongocat_log_error("Failed to fork input monitoring process: %s",
                       strerror(errno));
    munmap(any_key_pressed, sizeof(atomic_int));
    munmap(last_key_code, sizeof(atomic_int));
    any_key_pressed = NULL;
    last_key_code = NULL;
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

    // Wait for child to terminate with timeout
    int status;
    int wait_attempts = 0;
    while (wait_attempts < 10) {
      pid_t result = waitpid(input_child_pid, &status, WNOHANG);
      if (result == input_child_pid) {
        bongocat_log_debug("Previous input monitoring process terminated");
        break;
      } else if (result == -1) {
        if (errno == ECHILD) {
          bongocat_log_debug(
              "Input child process already cleaned up by signal handler");
          break;
        } else {
          bongocat_log_warning("Error waiting for input child process: %s",
                               strerror(errno));
          break;
        }
      }

      usleep(100000);
      wait_attempts++;
    }

    if (wait_attempts >= 10) {
      bongocat_log_warning("Force killing previous input monitoring process");
      kill(input_child_pid, SIGKILL);
      waitpid(input_child_pid, &status, 0);
    }

    input_child_pid = -1;
  }

  // Reuse shared memory if it exists, otherwise allocate new
  bool need_new_shm =
      (any_key_pressed == NULL || any_key_pressed == MAP_FAILED);

  if (need_new_shm) {
    any_key_pressed =
        (atomic_int *)mmap(NULL, sizeof(atomic_int), PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (any_key_pressed == MAP_FAILED) {
      bongocat_log_error("Failed to create shared memory for input: %s",
                         strerror(errno));
      return BONGOCAT_ERROR_MEMORY;
    }
    atomic_store(any_key_pressed, 0);
  }

  bool need_new_key_shm =
      (last_key_code == NULL || last_key_code == MAP_FAILED);

  if (need_new_key_shm) {
    last_key_code =
        (atomic_int *)mmap(NULL, sizeof(atomic_int), PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (last_key_code == MAP_FAILED) {
      bongocat_log_error("Failed to create shared memory for key code: %s",
                         strerror(errno));
      if (need_new_shm) {
        munmap(any_key_pressed, sizeof(atomic_int));
        any_key_pressed = NULL;
      }
      return BONGOCAT_ERROR_MEMORY;
    }
    atomic_store(last_key_code, 0);
  }

  // Fork new process
  input_child_pid = fork();
  if (input_child_pid < 0) {
    bongocat_log_error("Failed to fork input monitoring process: %s",
                       strerror(errno));
    if (need_new_shm) {
      munmap(any_key_pressed, sizeof(atomic_int));
      any_key_pressed = NULL;
    }
    if (need_new_key_shm) {
      munmap(last_key_code, sizeof(atomic_int));
      last_key_code = NULL;
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

    int status;
    int wait_attempts = 0;
    while (wait_attempts < 10) {
      pid_t result = waitpid(input_child_pid, &status, WNOHANG);
      if (result == input_child_pid) {
        break;
      } else if (result == -1) {
        break;
      }
      usleep(100000);
      wait_attempts++;
    }

    if (wait_attempts >= 10) {
      bongocat_log_warning("Force killing input monitoring child process");
      kill(input_child_pid, SIGKILL);
      waitpid(input_child_pid, &status, 0);
    }

    input_child_pid = -1;
  }

  // Cleanup shared memory
  if (any_key_pressed && any_key_pressed != MAP_FAILED) {
    munmap(any_key_pressed, sizeof(atomic_int));
    any_key_pressed = NULL;
  }
  if (last_key_code && last_key_code != MAP_FAILED) {
    munmap(last_key_code, sizeof(atomic_int));
    last_key_code = NULL;
  }

  bongocat_log_debug("Input monitoring cleanup complete");
}
