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
#include <signal.h>
#include <stdatomic.h>

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <wayland-client.h>

#include "../lib/stb_image.h"

// Version
#define BONGOCAT_VERSION "1.2.3"

// Common constants
#define DEFAULT_SCREEN_WIDTH 1920
#define DEFAULT_BAR_HEIGHT 40


// Config watcher constants
#define INOTIFY_EVENT_SIZE (sizeof(struct inotify_event))
#define INOTIFY_BUF_LEN (1024 * (INOTIFY_EVENT_SIZE + 16))

// Config watcher structure
typedef struct {
    int inotify_fd;
    int watch_fd;
    char *config_path;

    void (*reload_callback)(const char *config_path);

    pthread_t watcher_thread;
    atomic_bool _running;
} config_watcher_t;

// Config watcher function declarations
int config_watcher_init(config_watcher_t *watcher, const char *config_path, void (*callback)(const char *));
void config_watcher_start(config_watcher_t *watcher);
void config_watcher_stop(config_watcher_t *watcher);
void config_watcher_cleanup(config_watcher_t *watcher);


// Globals (Context)
typedef struct {
    int *any_key_pressed;

    pid_t _input_child_pid;
    atomic_bool _capture_input_running;
} input_context_t;

#endif // BONGOCAT_H