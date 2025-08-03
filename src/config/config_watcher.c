#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "core/bongocat.h"
#include "utils/error.h"
#include "config/config.h"
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/select.h>
#include <assert.h>

static void *config_watcher_thread(void *arg) {
    assert(arg);

    config_watcher_t *watcher = (config_watcher_t *)arg;
    char buffer[INOTIFY_BUF_LEN];
    time_t last_reload_time = 0;
    
    bongocat_log_info("Config watcher started for: %s", watcher->config_path);

    atomic_store(&watcher->_running, true);
    while (atomic_load(&watcher->_running)) {
        fd_set read_fds;
        struct timeval timeout;
        
        FD_ZERO(&read_fds);
        FD_SET(watcher->inotify_fd, &read_fds);
        
        // Set timeout to 1 second to allow checking watching flag
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        int select_result = select(watcher->inotify_fd + 1, &read_fds, NULL, NULL, &timeout);
        
        if (select_result < 0) {
            if (errno == EINTR) continue;
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
                bongocat_log_error("Config watcher read failed: %s", strerror(errno));
                continue;
            }
            
            bool should_reload = false;
            ssize_t i = 0;
            while (i < length) {
                struct inotify_event *event = (struct inotify_event *)&buffer[i];
                
                if (event->mask & (IN_MODIFY | IN_MOVED_TO)) {
                    should_reload = true;
                }
                
                i += INOTIFY_EVENT_SIZE + event->len;
            }
            
            // Debounce: only reload if at least 200ms have passed since last reload
            if (should_reload) {
                time_t current_time = time(NULL);
                if (current_time - last_reload_time >= 1) { // 1 second debounce
                    bongocat_log_info("Config file changed, reloading...");
                    last_reload_time = current_time;
                    
                    // Small delay to ensure file write is complete
                    usleep(100000); // 100ms
                    
                    if (watcher->reload_callback) {
                        watcher->reload_callback(watcher->config_path);
                    }
                }
            }
        }
    }
    atomic_store(&watcher->_running, false);
    
    bongocat_log_info("Config watcher stopped");
    return NULL;
}

int config_watcher_init(config_watcher_t *watcher, const char *config_path, void (*callback)(const char *)) {
    if (!watcher || !config_path || !callback) {
        return -1;
    }
    
    memset(watcher, 0, sizeof(config_watcher_t));
    
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
        return -1;
    }
    
    // Add watch for the config file
    watcher->watch_fd = inotify_add_watch(watcher->inotify_fd, config_path, 
                                         IN_MODIFY | IN_MOVED_TO | IN_CREATE);
    if (watcher->watch_fd < 0) {
        bongocat_log_error("Failed to add inotify watch for %s: %s", config_path, strerror(errno));
        free(watcher->config_path);
        close(watcher->inotify_fd);
        return -1;
    }
    
    watcher->reload_callback = callback;
    
    return 0;
}

void config_watcher_start(config_watcher_t *watcher) {
    if (!watcher) {
        return;
    }

    if (pthread_create(&watcher->watcher_thread, NULL, config_watcher_thread, watcher) != 0) {
        atomic_store(&watcher->_running, false);
        bongocat_log_error("Failed to create config watcher thread: %s", strerror(errno));
        return;
    }
    
    bongocat_log_info("Config watcher thread started");
}

void config_watcher_stop(config_watcher_t *watcher) {
    if (!watcher) {
        return;
    }
    
    atomic_store(&watcher->_running, false);
    //pthread_cancel(watcher->watcher_thread);
    // Wait for thread to finish
    if (pthread_join(watcher->watcher_thread, NULL) != 0) {
        bongocat_log_error("Failed to join config watcher thread: %s", strerror(errno));
    }
}

void config_watcher_cleanup(config_watcher_t *watcher) {
    if (!watcher) {
        return;
    }
    
    config_watcher_stop(watcher);
    
    if (watcher->watch_fd >= 0) {
        inotify_rm_watch(watcher->inotify_fd, watcher->watch_fd);
    }
    
    if (watcher->inotify_fd >= 0) {
        close(watcher->inotify_fd);
    }
    
    if (watcher->config_path) {
        free(watcher->config_path);
        watcher->config_path = NULL;
    }
    
    memset(watcher, 0, sizeof(config_watcher_t));
}