#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "config/config.h"
#include "core/bongocat.h"
#include "utils/error.h"

#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

static int config_watcher_add_watch(ConfigWatcher *watcher, bool log_errors) {
  if (!watcher || watcher->inotify_fd < 0 || !watcher->config_path) {
    return -1;
  }

  int fd = inotify_add_watch(watcher->inotify_fd, watcher->config_path,
                             IN_CLOSE_WRITE | IN_MODIFY | IN_MOVED_TO |
                                 IN_ATTRIB | IN_MOVE_SELF | IN_DELETE_SELF);
  if (fd < 0 && log_errors) {
    bongocat_log_error("Failed to add inotify watch for %s: %s",
                       watcher->config_path, strerror(errno));
  }

  watcher->watch_fd = fd;
  return fd;
}

static long long config_watcher_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void *config_watcher_thread(void *arg) {
  ConfigWatcher *watcher = (ConfigWatcher *)arg;
  char buffer[INOTIFY_BUF_LEN];
  long long last_reload_ms = 0;

  bongocat_log_info("Config watcher started for: %s", watcher->config_path);

  while (watcher->watching) {
    if (watcher->inotify_fd < 0) {
      break;
    }

    fd_set read_fds;
    struct timeval timeout;

    FD_ZERO(&read_fds);
    FD_SET(watcher->inotify_fd, &read_fds);

    // Set timeout to 1 second to allow checking watching flag
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    int select_result =
        select(watcher->inotify_fd + 1, &read_fds, NULL, NULL, &timeout);

    if (select_result < 0) {
      if (errno == EINTR)
        continue;
      bongocat_log_error("Config watcher select failed: %s", strerror(errno));
      break;
    }

    if (select_result == 0) {
      // Timeout, continue to check watching flag
      continue;
    }

    if (FD_ISSET(watcher->inotify_fd, &read_fds)) {
      ssize_t length = read(watcher->inotify_fd, buffer, INOTIFY_BUF_LEN);

      if (length < 0) {
        if (errno == EINTR || errno == EAGAIN) {
          continue;
        }
        bongocat_log_error("Config watcher read failed: %s", strerror(errno));
        continue;
      }

      bool should_reload = false;
      bool watch_invalidated = false;
      ssize_t i = 0;
      while (i < length) {
        struct inotify_event *event = (struct inotify_event *)&buffer[i];

        if (event->wd == watcher->watch_fd &&
            (event->mask &
             (IN_CLOSE_WRITE | IN_MODIFY | IN_MOVED_TO | IN_ATTRIB))) {
          should_reload = true;
        }

        if (event->wd == watcher->watch_fd &&
            (event->mask & (IN_MOVE_SELF | IN_DELETE_SELF | IN_IGNORED))) {
          watch_invalidated = true;
        }

        i += INOTIFY_EVENT_SIZE + event->len;
      }

      // File can be replaced atomically; re-register watch on the new inode.
      if (watch_invalidated && watcher->watching) {
        watcher->watch_fd = -1;
        bool rewatch_ok = false;
        for (int retry = 0; retry < 20 && watcher->watching; retry++) {
          if (config_watcher_add_watch(watcher, false) >= 0) {
            rewatch_ok = true;
            bongocat_log_debug("Re-armed config file watcher");
            break;
          }
          usleep(100000);  // 100ms retry interval
        }

        if (!rewatch_ok) {
          bongocat_log_warning(
              "Config watcher lost file watch; hot-reload may stop working");
        }
      }

      // Debounce reloads (300ms)
      if (should_reload) {
        long long current_ms = config_watcher_now_ms();
        if (current_ms - last_reload_ms >= 300) {
          bongocat_log_info("Config file changed, reloading...");
          last_reload_ms = current_ms;

          // Small delay to ensure file write is complete
          usleep(100000);  // 100ms

          if (watcher->reload_callback) {
            watcher->reload_callback(watcher->config_path);
          }
        }
      }
    }
  }

  bongocat_log_info("Config watcher stopped");
  return NULL;
}

int config_watcher_init(ConfigWatcher *watcher, const char *config_path,
                        void (*callback)(const char *)) {
  if (!watcher || !config_path || !callback) {
    return -1;
  }

  memset(watcher, 0, sizeof(ConfigWatcher));
  watcher->inotify_fd = -1;
  watcher->watch_fd = -1;

  // Initialize inotify
  watcher->inotify_fd = inotify_init1(IN_NONBLOCK);
  if (watcher->inotify_fd < 0) {
    bongocat_log_error("Failed to initialize inotify: %s", strerror(errno));
    return -1;
  }

  // Store config path
  watcher->config_path = strdup(config_path);
  if (!watcher->config_path) {
    close(watcher->inotify_fd);
    watcher->inotify_fd = -1;
    return -1;
  }

  if (config_watcher_add_watch(watcher, true) < 0) {
    free(watcher->config_path);
    watcher->config_path = NULL;
    close(watcher->inotify_fd);
    watcher->inotify_fd = -1;
    return -1;
  }

  watcher->reload_callback = callback;
  watcher->watching = false;

  return 0;
}

void config_watcher_start(ConfigWatcher *watcher) {
  if (!watcher || watcher->watching || watcher->inotify_fd < 0 ||
      watcher->watch_fd < 0) {
    return;
  }

  watcher->watching = true;

  if (pthread_create(&watcher->watcher_thread, NULL, config_watcher_thread,
                     watcher) != 0) {
    bongocat_log_error("Failed to create config watcher thread: %s",
                       strerror(errno));
    watcher->watching = false;
    return;
  }

  bongocat_log_info("Config watcher thread started");
}

void config_watcher_stop(ConfigWatcher *watcher) {
  if (!watcher || !watcher->watching) {
    return;
  }

  watcher->watching = false;

  // Wait for thread to finish
  if (pthread_join(watcher->watcher_thread, NULL) != 0) {
    bongocat_log_error("Failed to join config watcher thread: %s",
                       strerror(errno));
  }
}

void config_watcher_cleanup(ConfigWatcher *watcher) {
  if (!watcher) {
    return;
  }

  config_watcher_stop(watcher);

  if (watcher->inotify_fd >= 0 && watcher->watch_fd >= 0) {
    inotify_rm_watch(watcher->inotify_fd, watcher->watch_fd);
    watcher->watch_fd = -1;
  }

  if (watcher->inotify_fd >= 0) {
    close(watcher->inotify_fd);
    watcher->inotify_fd = -1;
  }

  if (watcher->config_path) {
    free(watcher->config_path);
    watcher->config_path = NULL;
  }

  memset(watcher, 0, sizeof(ConfigWatcher));
  watcher->inotify_fd = -1;
  watcher->watch_fd = -1;
}
