#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "core/bongocat.h"
#include "platform/wayland.h"
#include "graphics/animation.h"
#include "platform/input.h"
#include "config/config.h"
#include "utils/error.h"
#include "utils/memory.h"
#include <signal.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <sys/file.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/signalfd.h>

// =============================================================================
// GLOBAL STATE AND CONFIGURATION
// =============================================================================

static volatile sig_atomic_t running = 1;
static config_t g_config;
static ConfigWatcher g_config_watcher;
static char* current_config_file = NULL;

#define PID_FILE "/tmp/bongocat.pid"
#define DEFAULT_BONGOCAT_CONF "bongocat.conf";

// =============================================================================
// COMMAND LINE ARGUMENTS STRUCTURE
// =============================================================================

typedef struct {
    const char *config_file;
    bool watch_config;
    bool toggle_mode;
    bool show_help;
    bool show_version;
} cli_args_t;

// =============================================================================
// PROCESS MANAGEMENT MODULE
// =============================================================================

static int process_create_pid_file(void) {
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

static void process_remove_pid_file(void) {
    unlink(PID_FILE);
}

static pid_t process_get_running_pid(void) {
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
    process_remove_pid_file();
    return -1;
}

static int process_handle_toggle(void) {
    pid_t running_pid = process_get_running_pid();
    
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

// =============================================================================
// CONFIGURATION MANAGEMENT MODULE
// =============================================================================

static bool config_devices_changed(const config_t *old_config, const config_t *new_config) {
    if (old_config->num_keyboard_devices != new_config->num_keyboard_devices) {
        return true;
    }
    
    // Check if any device paths changed
    for (int i = 0; i < new_config->num_keyboard_devices; i++) {
        bool found = false;
        for (int j = 0; j < old_config->num_keyboard_devices; j++) {
            if (strcmp(new_config->keyboard_devices[i], old_config->keyboard_devices[j]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return true;
        }
    }
    
    return false;
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
    
    // If successful, check if input devices changed before updating config
    bool devices_changed = config_devices_changed(&g_config, &temp_config);
    
    // Clean up old output_name if it exists and is different
    if (g_config.output_name && temp_config.output_name && 
        strcmp(g_config.output_name, temp_config.output_name) != 0) {
        free(g_config.output_name);
    } else if (g_config.output_name && !temp_config.output_name) {
        free(g_config.output_name);
    }
    
    // Update the global config
    g_config = temp_config;
    
    // Update the running systems with new config
    wayland_update_config(&g_config);
    
    // Check if input devices changed and restart monitoring if needed
    if (devices_changed) {
        bongocat_log_info("Input devices changed, restarting input monitoring");
        bongocat_error_t input_result = input_restart_monitoring(g_config.keyboard_devices, 
                                                                g_config.num_keyboard_devices, 
                                                                g_config.enable_debug);
        if (input_result != BONGOCAT_SUCCESS) {
            bongocat_log_error("Failed to restart input monitoring: %s", bongocat_error_string(input_result));
        } else {
            bongocat_log_info("Input monitoring restarted successfully");
        }
    }
    
    bongocat_log_info("Configuration reloaded successfully!");
    bongocat_log_info("New screen dimensions: %dx%d", g_config.screen_width, g_config.bar_height);
}
static void current_config_reload_callback() {
    config_reload_callback(current_config_file);
}

static bongocat_error_t config_setup_watcher(const char *config_file) {
    const char *watch_path = config_file ? config_file : DEFAULT_BONGOCAT_CONF;
    
    if (config_watcher_init(&g_config_watcher, watch_path, config_reload_callback) == 0) {
        config_watcher_start(&g_config_watcher);
        bongocat_log_info("Config file watching enabled for: %s", watch_path);
        return BONGOCAT_SUCCESS;
    } else {
        bongocat_log_warning("Failed to initialize config watcher, continuing without hot-reload");
        return BONGOCAT_ERROR_CONFIG;
    }
}

// =============================================================================
// SYSTEM INITIALIZATION AND CLEANUP MODULE
// =============================================================================

static bongocat_error_t system_initialize_components(void) {
    bongocat_error_t result;
    
    // Initialize Wayland
    result = wayland_init(&g_config);
    if (result != BONGOCAT_SUCCESS) {
        bongocat_log_error("Failed to initialize Wayland: %s", bongocat_error_string(result));
        return result;
    }
    
    // Initialize animation system
    result = animation_init(&g_config);
    if (result != BONGOCAT_SUCCESS) {
        bongocat_log_error("Failed to initialize animation system: %s", bongocat_error_string(result));
        return result;
    }
    
    // Start input monitoring
    result = input_start_monitoring(g_config.keyboard_devices, g_config.num_keyboard_devices, g_config.enable_debug);
    if (result != BONGOCAT_SUCCESS) {
        bongocat_log_error("Failed to start input monitoring: %s", bongocat_error_string(result));
        return result;
    }
    
    // Start animation thread
    result = animation_start();
    if (result != BONGOCAT_SUCCESS) {
        bongocat_log_error("Failed to start animation thread: %s", bongocat_error_string(result));
        return result;
    }
    
    return BONGOCAT_SUCCESS;
}

static void system_cleanup_and_exit(int exit_code) {
    bongocat_log_info("Performing cleanup...");
    
    // Remove PID file
    process_remove_pid_file();
    
    // Stop config watcher
    config_watcher_cleanup(&g_config_watcher);
    
    // Stop animation system
    animation_cleanup();
    
    // Cleanup Wayland
    wayland_cleanup();
    
    // Cleanup input system
    input_cleanup();
    
    // Cleanup configuration
    config_cleanup_full(&g_config);
    config_cleanup();
    
    // Print memory statistics in debug mode
    if (g_config.enable_debug) {
        memory_print_stats();
    }
    
#ifdef DEBUG
    memory_leak_check();
#endif
    
    bongocat_log_info("Cleanup complete, exiting with code %d", exit_code);
    exit(exit_code);
}

// =============================================================================
// SIGNAL HANDLING MODULE
// =============================================================================


static bongocat_error_t signal_setup_handlers(int *signal_fd) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGUSR2);

    // Block signals globally so they are only delivered via signalfd
    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
        bongocat_log_error("Failed to block signals: %s", strerror(errno));
        return BONGOCAT_ERROR_THREAD;
    }

    *signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (*signal_fd == -1) {
        bongocat_log_error("Failed to create signalfd: %s", strerror(errno));
        return -1;
    }

    return BONGOCAT_SUCCESS;
}

// =============================================================================
// COMMAND LINE PROCESSING MODULE
// =============================================================================

static void cli_show_help(const char *program_name) {
    printf("Bongo Cat Wayland Overlay\n");
    printf("Usage: %s [options]\n", program_name);
    printf("Options:\n");
    printf("  -h, --help            Show this help message\n");
    printf("  -v, --version         Show version information\n");
    printf("  -c, --config          Specify config file (default: bongocat.conf)\n");
    printf("  -w, --watch-config    Watch config file for changes and reload automatically\n");
    printf("  -t, --toggle          Toggle bongocat on/off (start if not running, stop if running)\n");
    printf("\nConfiguration is loaded from bongocat.conf in the current directory.\n");
}

static void cli_show_version(void) {
    printf("Bongo Cat Overlay v" BONGOCAT_VERSION "\n");
    printf("Built with fast optimizations\n");
}

static int cli_parse_arguments(int argc, char *argv[], cli_args_t *args) {
    // Initialize arguments with defaults
    *args = (cli_args_t){
        .config_file = NULL,
        .watch_config = false,
        .toggle_mode = false,
        .show_help = false,
        .show_version = false
    };
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            args->show_help = true;
        } else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            args->show_version = true;
        } else if (strcmp(argv[i], "--config") == 0 || strcmp(argv[i], "-c") == 0) {
            if (i + 1 < argc) {
                args->config_file = argv[i + 1];
                i++; // Skip the next argument since it's the config file path
            } else {
                bongocat_log_error("--config option requires a file path");
                return 1;
            }
        } else if (strcmp(argv[i], "--watch-config") == 0 || strcmp(argv[i], "-w") == 0) {
            args->watch_config = true;
        } else if (strcmp(argv[i], "--toggle") == 0 || strcmp(argv[i], "-t") == 0) {
            args->toggle_mode = true;
        } else {
            bongocat_log_warning("Unknown argument: %s", argv[i]);
        }
    }
    
    return 0;
}

// =============================================================================
// MAIN APPLICATION ENTRY POINT
// =============================================================================

int main(int argc, char *argv[]) {
    bongocat_error_t result;
    
    // Initialize error system early
    bongocat_error_init(1); // Enable debug initially
    
    bongocat_log_info("Starting Bongo Cat Overlay v" BONGOCAT_VERSION);
    
    // Parse command line arguments
    cli_args_t args;
    if (cli_parse_arguments(argc, argv, &args) != 0) {
        return 1;
    }
    
    // Handle help and version requests
    if (args.show_help) {
        cli_show_help(argv[0]);
        return 0;
    }
    
    if (args.show_version) {
        cli_show_version();
        return 0;
    }
    
    // Handle toggle mode
    if (args.toggle_mode) {
        int toggle_result = process_handle_toggle();
        if (toggle_result >= 0) {
            return toggle_result; // Either successfully toggled off or error
        }
        // toggle_result == -1 means continue with startup
    }

    // Setup signal handlers
    int signal_fd = -1;
    result = signal_setup_handlers(&signal_fd);
    if (result != BONGOCAT_SUCCESS) {
        bongocat_log_error("Failed to setup signal handlers: %s", bongocat_error_string(result));
        return 1;
    }
    
    // Create PID file to track this instance
    int pid_fd = process_create_pid_file();
    if (pid_fd == -2) {
        close(signal_fd);
        bongocat_log_error("Another instance of bongocat is already running");
        return 1;
    } else if (pid_fd < 0) {
        close(signal_fd);
        bongocat_log_error("Failed to create PID file");
        return 1;
    }
    
    // Load configuration
    result = load_config(&g_config, args.config_file);
    if (result != BONGOCAT_SUCCESS) {
        close(signal_fd);
        bongocat_log_error("Failed to load configuration: %s", bongocat_error_string(result));
        return 1;
    }
    
    bongocat_log_info("Screen dimensions: %dx%d", g_config.screen_width, g_config.bar_height);
    
    // Initialize config watcher if requested
    if (args.watch_config) {
        config_setup_watcher(args.config_file);
    }
    current_config_file = args.config_file;
    
    // Initialize all system components
    result = system_initialize_components();
    if (result != BONGOCAT_SUCCESS) {
        close(signal_fd);
        system_cleanup_and_exit(1);
    }
    
    bongocat_log_info("Bongo Cat Overlay started successfully");
    
    // Main Wayland event loop with graceful shutdown
    result = wayland_run(&running, signal_fd, current_config_reload_callback);
    if (result != BONGOCAT_SUCCESS) {
        close(signal_fd);
        bongocat_log_error("Wayland event loop error: %s", bongocat_error_string(result));
        system_cleanup_and_exit(1);
    }
    
    bongocat_log_info("Main loop exited, shutting down");
    close(signal_fd);
    system_cleanup_and_exit(0);
    
    return 0; // Never reached
}