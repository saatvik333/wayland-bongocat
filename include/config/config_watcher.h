#ifndef CONFIG_WATCHER_H
#define CONFIG_WATCHER_H

#include <stdbool.h>
#include <stdint.h>

// Watches a config file and its directory, so edits, atomic replacements and
// symlink retargeting all trigger a reload once changes settle for 300 ms.
// A zero-initialized watcher is inactive.
typedef struct {
  char *path;  // NULL while inactive
  char *directory;
  char *filename;
  int inotify_fd;
  int directory_watch;  // -1 while the directory is missing
  int file_watch;       // -1 while the file is missing
  int64_t reload_at;    // Monotonic microseconds; 0 when nothing is pending
  int64_t retry_at;     // When to retry missing watches; 0 when attached
} config_watcher_t;

[[nodiscard]] bool config_watcher_init(config_watcher_t *watcher,
                                       const char *path);
// Descriptor to poll; -1 while inactive.
int config_watcher_fd(const config_watcher_t *watcher);
// Reads pending events; true when a reload is due.
bool config_watcher_process(config_watcher_t *watcher);
// Next monotonic microsecond time process must run, or 0.
int64_t config_watcher_deadline(const config_watcher_t *watcher);
void config_watcher_cleanup(config_watcher_t *watcher);

#endif  // CONFIG_WATCHER_H
