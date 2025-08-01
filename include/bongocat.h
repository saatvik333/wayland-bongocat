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

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <wayland-client.h>

#include "../lib/stb_image.h"

// Version
#define BONGOCAT_VERSION "1.2.2"

// Common constants
#define BONGOCAT_NUM_FRAMES 4
#define DEFAULT_SCREEN_WIDTH 1920
#define DEFAULT_BAR_HEIGHT 40

// Idle 1, Idle 2, Angry, Down1, Happy, Eat1, Sleep1, Refuse, Down2 ~~, Eat2, Sleep2, Attack~~
// both-up, left-down, right-down, both-down, ...
#define MAX_NUM_FRAMES 12
#define MAX_DIGIMON_FRAMES 12

// Animations
#define TOTAL_DIGIMON_ANIMATIONS 1

// bongocat + digimons
#define TOTAL_ANIMATIONS (1+TOTAL_DIGIMON_ANIMATIONS)


// Config watcher constants
#define INOTIFY_EVENT_SIZE (sizeof(struct inotify_event))
#define INOTIFY_BUF_LEN (1024 * (INOTIFY_EVENT_SIZE + 16))

// Config watcher structure
typedef struct {
    int inotify_fd;
    int watch_fd;
    pthread_t watcher_thread;
    bool watching;
    char *config_path;
    void (*reload_callback)(const char *config_path);
} ConfigWatcher;

// Config watcher function declarations
int config_watcher_init(ConfigWatcher *watcher, const char *config_path, void (*callback)(const char *));
void config_watcher_start(ConfigWatcher *watcher);
void config_watcher_stop(ConfigWatcher *watcher);
void config_watcher_cleanup(ConfigWatcher *watcher);

#endif // BONGOCAT_H