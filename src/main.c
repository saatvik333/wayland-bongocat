#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "bongocat.h"
#include "wayland.h"
#include "animation.h"
#include "wayland.h"
#include "input.h"
#include "config.h"
#include "error.h"
#include "memory.h"
#include <signal.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <sys/file.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>

static volatile sig_atomic_t running = 1;

static config_t* g_config = NULL;
static config_watcher_t* g_config_watcher = NULL;

static input_context_t* g_input_ctx = NULL;
static animation_context_t* g_animation_ctx = NULL;
static wayland_context_t* g_wayland_ctx = NULL;

static void cleanup_globals() {
    if (g_wayland_ctx) bongocat_free(g_wayland_ctx);
    if (g_animation_ctx) bongocat_free(g_animation_ctx);
    if (g_input_ctx) bongocat_free(g_input_ctx);
    if (g_config_watcher) bongocat_free(g_config_watcher);
    if (g_config) bongocat_free(g_config);
    g_wayland_ctx = NULL;
    g_animation_ctx = NULL;
    g_input_ctx = NULL;
    g_config_watcher = NULL;
    g_config = NULL;
}

#define PID_FILE "/tmp/bongocat.pid"

static int create_pid_file(void) {
    int fd = open(PID_FILE, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        bongocat_log_error("Failed to create PID file: %s", strerror(errno));
        return -1;
    }
    
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        close(fd);
        if (errno == EWOULDBLOCK) {
            bongocat_log_info("Another instance is already running");
            return -2; // Already running
        }
        bongocat_log_error("Failed to lock PID file: %s", strerror(errno));
        return -1;
    }
    
    char pid_str[32];
    snprintf(pid_str, sizeof(pid_str), "%d\n", getpid());
    if (write(fd, pid_str, strlen(pid_str)) < 0) {
        bongocat_log_error("Failed to write PID to file: %s", strerror(errno));
        close(fd);
        return -1;
    }
    
    return fd; // Keep file descriptor open to maintain lock
}

static void remove_pid_file(void) {
    unlink(PID_FILE);
}

static pid_t get_running_pid(void) {
    int fd = open(PID_FILE, O_RDONLY);
    if (fd < 0) {
        return -1; // No PID file exists
    }
    
    // Try to get a shared lock to read the file
    if (flock(fd, LOCK_SH | LOCK_NB) < 0) {
        close(fd);
        if (errno == EWOULDBLOCK) {
            // File is locked by another process, so it's running
            // We need to read the PID anyway, so let's try without lock
            fd = open(PID_FILE, O_RDONLY);
            if (fd < 0) return -1;
        } else {
            return -1;
        }
    }
    
    char pid_str[32];
    ssize_t bytes_read = read(fd, pid_str, sizeof(pid_str) - 1);
    close(fd);
    
    if (bytes_read <= 0) {
        return -1;
    }
    
    pid_str[bytes_read] = '\0';
    pid_t pid = (pid_t)atoi(pid_str);
    
    if (pid <= 0) {
        return -1;
    }
    
    // Check if process is actually running
    if (kill(pid, 0) == 0) {
        return pid; // Process is running
    }
    
    // Process is not running, remove stale PID file
    remove_pid_file();
    return -1;
}

static int handle_toggle(void) {
    pid_t running_pid = get_running_pid();
    
    if (running_pid > 0) {
        // Process is running, kill it
        bongocat_log_info("Stopping bongocat (PID: %d)", running_pid);
        if (kill(running_pid, SIGTERM) == 0) {
            // Wait a bit for graceful shutdown
            for (int i = 0; i < 50; i++) { // Wait up to 5 seconds
                if (kill(running_pid, 0) != 0) {
                    bongocat_log_info("Bongocat stopped successfully");
                    return 0;
                }
                usleep(100000); // 100ms
            }
            
            // Force kill if still running
            bongocat_log_warning("Force killing bongocat");
            kill(running_pid, SIGKILL);
            bongocat_log_info("Bongocat force stopped");
        } else {
            bongocat_log_error("Failed to stop bongocat: %s", strerror(errno));
            return 1;
        }
    } else {
        bongocat_log_info("Bongocat is not running, starting it now");
        return -1; // Signal to continue with normal startup
    }
    
    return 0;
}

static void signal_handler(int sig) {
    switch (sig) {
        case SIGINT:
        case SIGTERM:
            g_input_ctx->_capture_input_running = 0;
            g_animation_ctx->_running = 0;
            running = 0;
            bongocat_log_info("Received signal %d, shutting down gracefully", sig);
            break;
        case SIGCHLD:
            // Handle child process termination
            while (waitpid(-1, NULL, WNOHANG) > 0);
            break;
        default:
            bongocat_log_warning("Received unexpected signal %d", sig);
            break;
    }
}

static void config_reload_callback(const char *config_path) {
    bongocat_log_info("Reloading configuration from: %s", config_path);
    
    // Create a temporary config to test loading
    config_t temp_config;
    bongocat_error_t result = load_config(&temp_config, config_path);
    
    if (result != BONGOCAT_SUCCESS) {
        bongocat_log_error("Failed to reload config: %s", bongocat_error_string(result));
        bongocat_log_info("Keeping current configuration");
        return;
    }
    
    // If successful, update the global config
    const config_t old_config = *g_config;
    *g_config = temp_config;
    
    // Update the running systems with new config
    wayland_update_config(g_wayland_ctx, g_config, g_animation_ctx);
    
    // Check if input devices changed and restart monitoring if needed
    bool devices_changed = false;
    if (old_config.num_keyboard_devices != g_config->num_keyboard_devices) {
        devices_changed = true;
    } else {
        // Check if any device paths changed
        for (int i = 0; i < g_config->num_keyboard_devices; i++) {
            bool found = false;
            for (int j = 0; j < old_config.num_keyboard_devices; j++) {
                if (strcmp(g_config->keyboard_devices[i], old_config.keyboard_devices[j]) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                devices_changed = true;
                break;
            }
        }
    }
    
    if (devices_changed) {
        bongocat_log_info("Input devices changed, restarting input monitoring");
        bongocat_error_t input_result = input_restart_monitoring(g_input_ctx,
                                                                 g_config->keyboard_devices,
                                                                 g_config->num_keyboard_devices,
                                                                 g_config->enable_debug);
        if (input_result != BONGOCAT_SUCCESS) {
            bongocat_log_error("Failed to restart input monitoring: %s", bongocat_error_string(input_result));
        } else {
            bongocat_log_info("Input monitoring restarted successfully");
        }
    }
    
    bongocat_log_info("Configuration reloaded successfully!");
    bongocat_log_info("New screen dimensions: %dx%d", g_config->screen_width, g_config->bar_height);
}

static bongocat_error_t setup_signal_handlers(void) {
    struct sigaction sa;
    
    // Setup signal handler
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        bongocat_log_error("Failed to setup SIGINT handler: %s", strerror(errno));
        return BONGOCAT_ERROR_THREAD;
    }
    
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        bongocat_log_error("Failed to setup SIGTERM handler: %s", strerror(errno));
        return BONGOCAT_ERROR_THREAD;
    }
    
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        bongocat_log_error("Failed to setup SIGCHLD handler: %s", strerror(errno));
        return BONGOCAT_ERROR_THREAD;
    }
    
    // Ignore SIGPIPE
    signal(SIGPIPE, SIG_IGN);
    
    return BONGOCAT_SUCCESS;
}

static void cleanup_and_exit(int exit_code) {
    bongocat_log_info("Performing cleanup...");
    
    // Remove PID file
    remove_pid_file();
    
    // Stop config watcher
    config_watcher_cleanup(g_config_watcher);
    
    // Stop animation system
    animation_cleanup(g_animation_ctx);
    
    // Cleanup Wayland
    wayland_cleanup(g_wayland_ctx);
    
    // Cleanup input system
    input_cleanup(g_input_ctx);
    
    // Cleanup configuration
    config_cleanup(g_config);
    
    // Print memory statistics in debug mode
    if (g_config->enable_debug) {
        memory_print_stats();
    }

    cleanup_globals();
    
#ifdef DEBUG
    memory_leak_check();
#endif
    
    bongocat_log_info("Cleanup complete, exiting with code %d", exit_code);
    exit(exit_code);
}

int main(int argc, char *argv[]) {
    bongocat_error_t result;
    
    // Initialize error system early
    bongocat_error_init(1); // Enable debug initially
    
    bongocat_log_info("Starting Bongo Cat Overlay v" BONGOCAT_VERSION);
    
    // Parse command line arguments
    const char *config_file = NULL;
    bool watch_config = false;
    bool toggle_mode = false;

    // Allocate and initialize config_t
    g_config = bongocat_malloc(sizeof(config_t));
    if (!g_config) {
        bongocat_log_error("Failed to allocate memory for g_config");
        return EXIT_FAILURE;
    }
    *g_config = (config_t){
        .screen_width = DEFAULT_SCREEN_WIDTH,
        .bar_height = DEFAULT_BAR_HEIGHT,
        .keyboard_devices = NULL,
        .num_keyboard_devices = 0,
        .cat_x_offset = 100,
        .cat_y_offset = 10,
        .cat_height = 40,
        .overlay_height = 50,
        .idle_frame = 0,
        .keypress_duration = 100,
        .test_animation_duration = 200,
        .test_animation_interval = 3,
        .fps = 60,
        .overlay_opacity = 150,
        .enable_debug = 1,
        .overlay_position = POSITION_TOP,

        .animation_index = 0,
        .invert_color = 0,
        .crop_sprite = 0,
        .padding_x = 0,
        .padding_y = 0,

        ._config_keyboard_devices = NULL,
        ._config_num_devices = 0,
    };

    // Config watcher
    g_config_watcher = bongocat_malloc(sizeof(config_watcher_t));
    if (!g_config_watcher) {
        bongocat_free(g_config);
        bongocat_log_error("Failed to allocate memory for g_config_watcher");
        return EXIT_FAILURE;
    }
    *g_config_watcher = (config_watcher_t){
        .inotify_fd = 0,
        .watch_fd = 0,
        .config_path = NULL,
        .reload_callback = NULL,
        .watcher_thread = 0,
        ._running = 0,
    };

    // Input context
    g_input_ctx = bongocat_malloc(sizeof(input_context_t));
    if (!g_input_ctx) {
        bongocat_free(g_config);
        bongocat_free(g_config_watcher);
        bongocat_log_error("Failed to allocate memory for g_input_ctx");
        return EXIT_FAILURE;
    }
    *g_input_ctx = (input_context_t){
        .any_key_pressed = NULL,
        ._input_child_pid = -1,
    };

    // Animation context
    g_animation_ctx = bongocat_malloc(sizeof(animation_context_t));
    if (!g_animation_ctx) {
        bongocat_free(g_input_ctx);
        bongocat_free(g_config_watcher);
        bongocat_free(g_config);
        bongocat_log_error("Failed to allocate memory for g_animation_ctx");
        return EXIT_FAILURE;
    }
    *g_animation_ctx = (animation_context_t){
        .anim_frame_index = 0,
        ._running = 0,
        ._anim_thread = 0,
    };

    // Wayland context
    g_wayland_ctx = bongocat_malloc(sizeof(wayland_context_t));
    if (!g_wayland_ctx) {
        bongocat_free(g_animation_ctx);
        bongocat_free(g_input_ctx);
        bongocat_free(g_config_watcher);
        bongocat_free(g_config);
        bongocat_log_error("Failed to allocate memory for g_wayland_ctx");
        return EXIT_FAILURE;
    }
    *g_wayland_ctx = (wayland_context_t){
        .display = NULL,
        .compositor = NULL,
        .shm = NULL,
        .layer_shell = NULL,
        .output = NULL,
        .surface = NULL,
        .buffer = NULL,
        .layer_surface = NULL,
        .pixels = NULL,

        .configured = false,

        .current_config = g_config,

        ._wayland_screen_width = 0,
        ._wayland_screen_height = 0,
        ._wayland_transform = WL_OUTPUT_TRANSFORM_NORMAL,
        ._wayland_raw_width = 0,
        ._wayland_raw_height = 0,
        ._wayland_mode_received = false,
        ._wayland_geometry_received = false,
    };
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("V-Pet/Bongo Cat Wayland Overlay\n");
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  -h, --help         Show this help message\n");
            printf("  -v, --version      Show version information\n");
            printf("  -c, --config       Specify config file (default: bongocat.conf)\n");
            printf("  -w, --watch-config Watch config file for changes and reload automatically\n");
            printf("  --toggle           Toggle bongocat on/off (start if not running, stop if running)\n");
            printf("\nConfiguration is loaded from bongocat.conf in the current directory.\n");
            return 0;
        } else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("Bongo Cat Overlay v" BONGOCAT_VERSION "\n");
            printf("Built with fast optimizations\n");
            return 0;
        } else if (strcmp(argv[i], "--config") == 0 || strcmp(argv[i], "-c") == 0) {
            if (i + 1 < argc) {
                config_file = argv[i + 1];
                i++; // Skip the next argument since it's the config file path
            } else {
                bongocat_log_error("--config option requires a file path");
                cleanup_globals();
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[i], "--watch-config") == 0 || strcmp(argv[i], "-w") == 0) {
            watch_config = true;
        } else if (strcmp(argv[i], "--toggle") == 0) {
            toggle_mode = true;
        } else {
            bongocat_log_warning("Unknown argument: %s", argv[i]);
        }
    }
    
    // Handle toggle mode
    if (toggle_mode) {
        int toggle_result = handle_toggle();
        if (toggle_result >= 0) {
            return toggle_result; // Either successfully toggled off or error
        }
        // toggle_result == -1 means continue with startup
    }
    
    // Setup signal handlers
    result = setup_signal_handlers();
    if (result != BONGOCAT_SUCCESS) {
        bongocat_log_error("Failed to setup signal handlers: %s", bongocat_error_string(result));
        cleanup_globals();
        return EXIT_FAILURE;
    }
    
    // Create PID file to track this instance
    int pid_fd = create_pid_file();
    if (pid_fd == -2) {
        bongocat_log_error("Another instance of bongocat is already running");
        cleanup_globals();
        return EXIT_FAILURE;
    } else if (pid_fd < 0) {
        bongocat_log_error("Failed to create PID file");
        cleanup_globals();
        return EXIT_FAILURE;
    }
    
    // Load configuration
    result = load_config(g_config, config_file);
    if (result != BONGOCAT_SUCCESS) {
        bongocat_log_error("Failed to load configuration: %s", bongocat_error_string(result));
        cleanup_globals();
        return EXIT_FAILURE;
    }
    
    bongocat_log_info("Screen dimensions: %dx%d", g_config->screen_width, g_config->bar_height);
    
    // Initialize config watcher if requested
    if (watch_config) {
        const char *watch_path = config_file ? config_file : "bongocat.conf";
        if (config_watcher_init(g_config_watcher, watch_path, config_reload_callback) == 0) {
            config_watcher_start(g_config_watcher);
            bongocat_log_info("Config file watching enabled for: %s", watch_path);
        } else {
            bongocat_log_warning("Failed to initialize config watcher, continuing without hot-reload");
        }
    }
    
    // Initialize Wayland
    /// @NOTE: animation context don't need to be ready, but wayland needs its reference for listeners
    result = wayland_init(g_wayland_ctx, g_animation_ctx, g_config);
    if (result != BONGOCAT_SUCCESS) {
        bongocat_log_error("Failed to initialize Wayland: %s", bongocat_error_string(result));
        cleanup_and_exit(1);
    }
    
    // Initialize animation system
    result = animation_init(g_animation_ctx, g_config);
    if (result != BONGOCAT_SUCCESS) {
        bongocat_log_error("Failed to initialize animation system: %s", bongocat_error_string(result));
        cleanup_and_exit(1);
    }
    
    // Start input monitoring
    result = input_start_monitoring(g_input_ctx, g_config->keyboard_devices, g_config->num_keyboard_devices, g_config->enable_debug);
    if (result != BONGOCAT_SUCCESS) {
        bongocat_log_error("Failed to start input monitoring: %s", bongocat_error_string(result));
        cleanup_and_exit(1);
    }

    // Validate Setup
    assert(g_wayland_ctx->current_config == g_config);
    
    // Start animation thread
    result = animation_start(g_animation_ctx, g_input_ctx, g_wayland_ctx, g_config);
    if (result != BONGOCAT_SUCCESS) {
        bongocat_log_error("Failed to start animation thread: %s", bongocat_error_string(result));
        cleanup_and_exit(1);
    }
    
    bongocat_log_info("Bongo Cat Overlay started successfully");
    
    // Main Wayland event loop with graceful shutdown
    result = wayland_run(g_wayland_ctx, &running);
    if (result != BONGOCAT_SUCCESS) {
        bongocat_log_error("Wayland event loop error: %s", bongocat_error_string(result));
        cleanup_and_exit(1);
    }
    
    bongocat_log_info("Main loop exited, shutting down");
    cleanup_and_exit(0);
    
    return 0; // Never reached
}