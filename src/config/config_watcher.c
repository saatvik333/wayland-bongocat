#define _POSIX_C_SOURCE 200809L
#include "core/bongocat.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((int64_t)ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

static void add_watch(ConfigWatcher *watcher) {
  watcher->watch_fd = inotify_add_watch(
      watcher->inotify_fd, watcher->directory,
      IN_CLOSE_WRITE | IN_MODIFY | IN_MOVED_TO | IN_MOVED_FROM | IN_CREATE |
          IN_DELETE | IN_ATTRIB | IN_MOVE_SELF | IN_DELETE_SELF);
  if (watcher->watch_fd >= 0 && watcher->watching) {
    watcher->reload_at_ms = now_ms() + 300;
  }
}

int config_watcher_init(ConfigWatcher *watcher, const char *path,
                        void (*callback)(const char *)) {
  if (!watcher || !path || !callback) {
    return -1;
  }
  *watcher = (ConfigWatcher){.inotify_fd = -1, .watch_fd = -1};
  watcher->config_path = strdup(path);
  watcher->directory = strdup(path);
  const char *slash = strrchr(path, '/');
  watcher->filename = strdup(slash ? slash + 1 : path);
  if (!watcher->config_path || !watcher->directory || !watcher->filename) {
    goto fail;
  }
  if (slash) {
    size_t length = (size_t)(slash - path);
    watcher->directory[length ? length : 1] = '\0';
  } else {
    free(watcher->directory);
    watcher->directory = strdup(".");
    if (!watcher->directory) {
      goto fail;
    }
  }
  watcher->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (watcher->inotify_fd < 0) {
    goto fail;
  }
  add_watch(watcher);
  watcher->reload_callback = callback;
  return 0;
fail:
  config_watcher_cleanup(watcher);
  return -1;
}

void config_watcher_start(ConfigWatcher *watcher) {
  if (watcher && watcher->inotify_fd >= 0) {
    watcher->watching = true;
  }
}

void config_watcher_stop(ConfigWatcher *watcher) {
  if (watcher) {
    watcher->watching = false;
  }
}

void config_watcher_process(ConfigWatcher *watcher) {
  if (!watcher || !watcher->watching) {
    return;
  }
  if (watcher->watch_fd < 0) {
    add_watch(watcher);
  }
  _Alignas(struct inotify_event) char buffer[INOTIFY_BUF_LEN];
  ssize_t length;
  while ((length = read(watcher->inotify_fd, buffer, sizeof(buffer))) > 0) {
    for (size_t i = 0; i + sizeof(struct inotify_event) <= (size_t)length;) {
      struct inotify_event *event = (void *)(buffer + i);
      size_t size = sizeof(*event) + event->len;
      if (size > (size_t)length - i) {
        break;
      }
      if (event->mask & IN_Q_OVERFLOW) {
        watcher->reload_at_ms = now_ms() + 300;
      }
      if (event->wd == watcher->watch_fd) {
        if (event->mask & (IN_MOVE_SELF | IN_DELETE_SELF | IN_IGNORED)) {
          if (!(event->mask & IN_IGNORED)) {
            inotify_rm_watch(watcher->inotify_fd, watcher->watch_fd);
          }
          watcher->watch_fd = -1;
          watcher->reload_at_ms = now_ms() + 300;
        } else if (event->len && strcmp(event->name, watcher->filename) == 0) {
          watcher->reload_at_ms = now_ms() + 300;
        }
      }
      i += size;
    }
  }
  if (watcher->reload_at_ms && now_ms() >= watcher->reload_at_ms) {
    watcher->reload_at_ms = 0;
    watcher->reload_callback(watcher->config_path);
  }
}

void config_watcher_cleanup(ConfigWatcher *watcher) {
  if (!watcher) {
    return;
  }
  config_watcher_stop(watcher);
  if (watcher->inotify_fd >= 0) {
    close(watcher->inotify_fd);
  }
  free(watcher->config_path);
  free(watcher->directory);
  free(watcher->filename);
  *watcher = (ConfigWatcher){.inotify_fd = -1, .watch_fd = -1};
}

int config_watcher_timeout(ConfigWatcher *watcher) {
  if (!watcher || !watcher->watching) {
    return -1;
  }
  if (watcher->watch_fd < 0) {
    return 1000;
  }
  if (!watcher->reload_at_ms) {
    return -1;
  }
  int64_t remaining = watcher->reload_at_ms - now_ms();
  return remaining > 0 ? (int)remaining : 0;
}
