#define _POSIX_C_SOURCE 200809L
#include "config/config_watcher.h"

#include "utils/clock.h"

#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#define SETTLE_US 300000   // Editors write in several steps
#define RETRY_US  1000000  // Re-adding watches on missing paths

// The file watch follows symlinks; the directory watch catches replacement of
// the configured name. Only entries of interest are watched: changes to
// unrelated files in the directory would otherwise wake the renderer.
static void watch_file(config_watcher_t *watcher) {
  int previous = watcher->file_watch;
  watcher->file_watch = inotify_add_watch(
      watcher->inotify_fd, watcher->path,
      IN_CLOSE_WRITE | IN_MODIFY | IN_ATTRIB | IN_MOVE_SELF | IN_DELETE_SELF);
  if (previous >= 0 && previous != watcher->file_watch) {
    inotify_rm_watch(watcher->inotify_fd, previous);
  }
}

static void watch_directory(config_watcher_t *watcher) {
  watcher->directory_watch = inotify_add_watch(
      watcher->inotify_fd, watcher->directory,
      IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_CREATE | IN_DELETE |
          IN_ATTRIB | IN_MOVE_SELF | IN_DELETE_SELF | IN_ONLYDIR);
}

static bool watches_missing(const config_watcher_t *watcher) {
  return watcher->directory_watch < 0 || watcher->file_watch < 0;
}

// Reattaches watches. A replaced file or retargeted symlink needs a new file
// watch; a missing path is retried periodically. Returns true when every
// watch was missing before and is attached now.
static bool rewatch(config_watcher_t *watcher, bool refresh, int64_t now) {
  bool missing = watches_missing(watcher);
  if (watcher->directory_watch < 0) {
    watch_directory(watcher);
  }
  if (refresh || missing) {
    watch_file(watcher);
  }
  watcher->retry_at = watches_missing(watcher) ? now + RETRY_US : 0;
  return missing && !watcher->retry_at;
}

bool config_watcher_init(config_watcher_t *watcher, const char *path) {
  const char *slash = strrchr(path, '/');
  *watcher = (config_watcher_t){
      .path = strdup(path),
      .filename = strdup(slash ? slash + 1 : path),
      .directory =
          slash ? strndup(path, slash == path ? 1 : (size_t)(slash - path))
                : strdup("."),
      .directory_watch = -1,
      .file_watch = -1,
  };
  watcher->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (!watcher->path || !watcher->filename || !watcher->directory ||
      watcher->inotify_fd < 0) {
    if (watcher->inotify_fd >= 0) {
      close(watcher->inotify_fd);
    }
    free(watcher->path);
    free(watcher->directory);
    free(watcher->filename);
    *watcher = (config_watcher_t){0};
    return false;
  }
  rewatch(watcher, true, monotonic_us());
  return true;
}

int config_watcher_fd(const config_watcher_t *watcher) {
  return watcher->path ? watcher->inotify_fd : -1;
}

bool config_watcher_process(config_watcher_t *watcher) {
  if (!watcher->path) {
    return false;
  }
  bool refresh = false;
  alignas(struct inotify_event) char buffer[4096];
  ssize_t length;
  while ((length = read(watcher->inotify_fd, buffer, sizeof(buffer))) > 0) {
    for (size_t i = 0; i + sizeof(struct inotify_event) <= (size_t)length;) {
      const struct inotify_event *event = (const void *)(buffer + i);
      i += sizeof(*event) + event->len;
      if (event->mask & IN_Q_OVERFLOW) {
        refresh = true;
      } else if (event->wd == watcher->file_watch) {
        if (event->mask & IN_IGNORED) {
          watcher->file_watch = -1;
        }
        refresh = true;
      } else if (event->wd == watcher->directory_watch) {
        if (event->mask & (IN_MOVE_SELF | IN_DELETE_SELF | IN_IGNORED)) {
          inotify_rm_watch(watcher->inotify_fd, watcher->directory_watch);
          watcher->directory_watch = -1;
          refresh = true;
        } else if (event->len && strcmp(event->name, watcher->filename) == 0) {
          refresh = true;
        }
      }
    }
  }
  int64_t now = monotonic_us();
  bool reattached =
      (refresh || (watcher->retry_at && now >= watcher->retry_at)) &&
      rewatch(watcher, refresh, now);
  // Changes may also have happened while a path was missing.
  if (refresh || reattached) {
    watcher->reload_at = now + SETTLE_US;
  }
  if (watcher->reload_at && now >= watcher->reload_at) {
    watcher->reload_at = 0;
    return true;
  }
  return false;
}

int64_t config_watcher_deadline(const config_watcher_t *watcher) {
  int64_t reload = watcher->reload_at;
  int64_t retry = watcher->retry_at;
  if (!reload || (retry && retry < reload)) {
    return retry;
  }
  return reload;
}

void config_watcher_cleanup(config_watcher_t *watcher) {
  if (watcher->path) {
    close(watcher->inotify_fd);
  }
  free(watcher->path);
  free(watcher->directory);
  free(watcher->filename);
  *watcher = (config_watcher_t){0};
}
