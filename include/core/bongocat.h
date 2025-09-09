#ifndef BONGOCAT_H
#define BONGOCAT_H

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <linux/input.h>
#include <sys/inotify.h>

#define _POSIX_C_SOURCE 200809L
#include <wayland-client.h>

#include "../lib/stb_image.h"

// Version
#define BONGOCAT_VERSION "1.2.5"

// Common constants
#define NUM_FRAMES 4
#define DEFAULT_SCREEN_WIDTH 1920
#define DEFAULT_BAR_HEIGHT 40
#define MAX_OUTPUTS 8 // Maximum monitor outputs to store
#define CAT_IMAGE_WIDTH 864
#define CAT_IMAGE_HEIGHT 360

#define BONGOCAT_FRAME_BOTH_UP 0
#define BONGOCAT_FRAME_LEFT_DOWN 1
#define BONGOCAT_FRAME_RIGHT_DOWN 2
#define BONGOCAT_FRAME_BOTH_DOWN 3

// Config watcher constants
#define INOTIFY_EVENT_SIZE (sizeof(struct inotify_event))
#define INOTIFY_BUF_LEN (1024 * (INOTIFY_EVENT_SIZE + 16))

// Config watcher structure
typedef struct {
    int inotify_fd;
    int watch_fd;
    int signal_fd;
    pthread_t watcher_thread;
    bool watching;
    char *config_path;
    void (*reload_callback)(const char *config_path);
} ConfigWatcher;

// Output monitor reference structure
typedef struct {
    struct wl_output *wl_output;
    struct zxdg_output_v1 *xdg_output;
    uint32_t name;         // Registry name
    char name_str[128];   // From xdg-output
    bool name_received;
} output_ref_t;

// Config watcher function declarations
int config_watcher_init(ConfigWatcher *watcher, int signal_fd, const char *config_path, void (*callback)(const char *));
void config_watcher_start(ConfigWatcher *watcher);
void config_watcher_stop(ConfigWatcher *watcher);
void config_watcher_cleanup(ConfigWatcher *watcher);

#endif // BONGOCAT_H