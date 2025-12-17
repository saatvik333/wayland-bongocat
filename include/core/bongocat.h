#ifndef BONGOCAT_H
#define BONGOCAT_H

// POSIX feature test macro - must be before includes
#define _POSIX_C_SOURCE 200809L

#include "../lib/stb_image.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <wayland-client.h>

// =============================================================================
// VERSION
// =============================================================================

#define BONGOCAT_VERSION "1.3.2"

// =============================================================================
// COMPILE-TIME CONSTANTS
// =============================================================================

// Display defaults
#define DEFAULT_SCREEN_WIDTH 1920
#define DEFAULT_BAR_HEIGHT   40
#define MAX_OUTPUTS          8  // Maximum monitor outputs to store
#define MAX_TOPLEVELS        512

// Frame constants
#define NUM_FRAMES       4
#define CAT_IMAGE_WIDTH  864
#define CAT_IMAGE_HEIGHT 360

// Frame indices
#define BONGOCAT_FRAME_BOTH_UP    0
#define BONGOCAT_FRAME_LEFT_DOWN  1
#define BONGOCAT_FRAME_RIGHT_DOWN 2
#define BONGOCAT_FRAME_BOTH_DOWN  3

// Inotify buffer sizing
#define INOTIFY_EVENT_SIZE (sizeof(struct inotify_event))
#define INOTIFY_BUF_LEN    (1024 * (INOTIFY_EVENT_SIZE + 16))

// =============================================================================
// TYPE DEFINITIONS
// =============================================================================

// Config watcher for hot-reload support
typedef struct {
  int inotify_fd;
  int watch_fd;
  pthread_t watcher_thread;
  bool watching;
  char *config_path;
  void (*reload_callback)(const char *config_path);
} ConfigWatcher;

// Output monitor reference for multi-monitor support
typedef struct {
  struct wl_output *wl_output;
  struct zxdg_output_v1 *xdg_output;
  uint32_t name;       // Registry name
  char name_str[128];  // From xdg-output
  bool name_received;
  int x, y;
  int width, height;
  int hypr_id;  // monitor ID in Hyprland
} output_ref_t;

// =============================================================================
// CONFIG WATCHER FUNCTIONS
// =============================================================================

// Initialize config watcher - returns 0 on success, -1 on failure
int config_watcher_init(ConfigWatcher *watcher, const char *config_path,
                        void (*callback)(const char *));

// Start watching for config changes
void config_watcher_start(ConfigWatcher *watcher);

// Stop watching for config changes
void config_watcher_stop(ConfigWatcher *watcher);

// Cleanup config watcher resources
void config_watcher_cleanup(ConfigWatcher *watcher);

#endif  // BONGOCAT_H