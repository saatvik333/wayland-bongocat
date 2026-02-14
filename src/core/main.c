#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "config/config.h"
#include "core/bongocat.h"
#include "core/multi_monitor.h"
#include "graphics/animation.h"
#include "platform/input.h"
#include "platform/wayland.h"
#include "utils/error.h"
#include "utils/memory.h"

#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

// =============================================================================
// GLOBAL STATE AND CONFIGURATION
// =============================================================================

static volatile sig_atomic_t running = 1;
static config_t g_config;
static ConfigWatcher g_config_watcher = {.inotify_fd = -1, .watch_fd = -1};
static bool g_manage_pid_file = true;
static const char *g_forced_monitor_name = NULL;
static _Atomic bool g_reload_pending = false;

#define PID_FILE "/tmp/bongocat.pid"

// =============================================================================
// COMMAND LINE ARGUMENTS STRUCTURE
// =============================================================================

typedef struct {
  const char *config_file;
  const char *monitor_name;  // --monitor override for multi-monitor children
  bool multi_monitor_child;  // Internal flag to skip PID file management
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
    int lock_err = errno;
    close(fd);
    if (lock_err == EWOULDBLOCK) {
      bongocat_log_info("Another instance is already running");
      return -2;  // Already running
    }
    bongocat_log_error("Failed to lock PID file: %s", strerror(lock_err));
    return -1;
  }

  char pid_str[32];
  snprintf(pid_str, sizeof(pid_str), "%d\n", getpid());
  if (write(fd, pid_str, strlen(pid_str)) < 0) {
    bongocat_log_error("Failed to write PID to file: %s", strerror(errno));
    close(fd);
    return -1;
  }

  return fd;  // Keep file descriptor open to maintain lock
}

static void process_remove_pid_file(void) {
  unlink(PID_FILE);
}

static pid_t process_get_running_pid(void) {
  int fd = open(PID_FILE, O_RDONLY);
  if (fd < 0) {
    return -1;  // No PID file exists
  }

  // Try to get a shared lock to read the file
  if (flock(fd, LOCK_SH | LOCK_NB) < 0) {
    int lock_err = errno;
    close(fd);
    if (lock_err == EWOULDBLOCK) {
      // File is locked by another process, so it's running
      // We need to read the PID anyway, so let's try without lock
      fd = open(PID_FILE, O_RDONLY);
      if (fd < 0)
        return -1;
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
    return pid;  // Process is running
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
    // Negate running pid to allow targetting process group (multiple monitors)
    if (kill(-running_pid, SIGTERM) == 0) {
      // Wait a bit for graceful shutdown
      for (int i = 0; i < 50; i++) {  // Wait up to 5 seconds
        if (kill(-running_pid, 0) != 0) {
          bongocat_log_info("Bongocat stopped successfully");
          return 0;
        }
        usleep(100000);  // 100ms
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
    return -1;  // Signal to continue with normal startup
  }

  return 0;
}

// =============================================================================
// SIGNAL HANDLING MODULE
// =============================================================================

static void signal_handler(int sig) {
  // Only async-signal-safe functions allowed here
  switch (sig) {
  case SIGINT:
  case SIGTERM:
  case SIGQUIT:
  case SIGHUP:
    running = 0;
    break;
  case SIGCHLD:
    while (waitpid(-1, NULL, WNOHANG) > 0)
      ;
    break;
  default:
    break;
  }
}

// Crash signal handler - only async-signal-safe operations
static void crash_signal_handler(int sig) {
  // Kill child process directly (async-signal-safe)
  pid_t child = input_get_child_pid();
  if (child > 0) {
    kill(child, SIGTERM);
  }

  // Remove PID file (unlink is async-signal-safe)
  if (g_manage_pid_file) {
    unlink(PID_FILE);
  }

  // Reset to default handler and re-raise
  signal(sig, SIG_DFL);
  raise(sig);
}

static bongocat_error_t signal_setup_handlers(void) {
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

  // Handle SIGQUIT (Ctrl+\) and SIGHUP (terminal hangup)
  if (sigaction(SIGQUIT, &sa, NULL) == -1) {
    bongocat_log_error("Failed to setup SIGQUIT handler: %s", strerror(errno));
    return BONGOCAT_ERROR_THREAD;
  }

  if (sigaction(SIGHUP, &sa, NULL) == -1) {
    bongocat_log_error("Failed to setup SIGHUP handler: %s", strerror(errno));
    return BONGOCAT_ERROR_THREAD;
  }

  // Ignore SIGPIPE
  signal(SIGPIPE, SIG_IGN);

  // Setup crash signal handlers to ensure child cleanup
  struct sigaction crash_sa;
  crash_sa.sa_handler = crash_signal_handler;
  sigemptyset(&crash_sa.sa_mask);
  crash_sa.sa_flags = SA_RESETHAND;  // Reset to default after handling

  sigaction(SIGSEGV, &crash_sa, NULL);
  sigaction(SIGABRT, &crash_sa, NULL);
  sigaction(SIGFPE, &crash_sa, NULL);
  sigaction(SIGILL, &crash_sa, NULL);

  return BONGOCAT_SUCCESS;
}

// =============================================================================
// CONFIGURATION MANAGEMENT MODULE
// =============================================================================

static void config_free_output_selection(config_t *config) {
  if (!config) {
    return;
  }

  if (config->output_name) {
    free(config->output_name);
    config->output_name = NULL;
  }

  if (config->output_names) {
    for (int i = 0; i < config->num_output_names; i++) {
      free(config->output_names[i]);
    }
    free(config->output_names);
    config->output_names = NULL;
  }

  config->num_output_names = 0;
}

static bongocat_error_t config_apply_forced_monitor(config_t *config,
                                                    const char *monitor_name) {
  if (!config || !monitor_name) {
    return BONGOCAT_ERROR_INVALID_PARAM;
  }

  config_free_output_selection(config);

  config->output_name = strdup(monitor_name);
  if (!config->output_name) {
    bongocat_log_error("Failed to allocate monitor override '%s'",
                       monitor_name);
    return BONGOCAT_ERROR_MEMORY;
  }

  bongocat_log_info("Using forced monitor output: '%s'", monitor_name);
  return BONGOCAT_SUCCESS;
}

static void config_reload_apply(const char *config_path) {
  bongocat_log_info("Reloading configuration from: %s", config_path);

  // Save old device info before loading so we can detect input-device changes.
  int old_num_devices = g_config.num_keyboard_devices;

  // Copy old device paths so we can compare after reload
  char **old_device_paths = NULL;
  if (old_num_devices > 0 && g_config.keyboard_devices != NULL) {
    old_device_paths = malloc(sizeof(char *) * old_num_devices);
    if (old_device_paths != NULL) {
      for (int i = 0; i < old_num_devices; i++) {
        old_device_paths[i] = g_config.keyboard_devices[i]
                                  ? strdup(g_config.keyboard_devices[i])
                                  : NULL;
      }
    }
  }

  // Create a temporary config to test loading
  config_t temp_config = {0};
  bongocat_error_t result = load_config(&temp_config, config_path);

  if (result != BONGOCAT_SUCCESS) {
    bongocat_log_error("Failed to reload config: %s",
                       bongocat_error_string(result));
    bongocat_log_info("Keeping current configuration");
    config_cleanup_full(&temp_config);
    // Free saved paths
    if (old_device_paths != NULL) {
      for (int i = 0; i < old_num_devices; i++) {
        free(old_device_paths[i]);
      }
      free(old_device_paths);
    }
    return;
  }

  // Check if devices changed (count or any path differs)
  bool devices_changed = (old_num_devices != temp_config.num_keyboard_devices);
  if (!devices_changed && old_device_paths != NULL) {
    // Same count - check if any paths differ
    for (int i = 0; i < old_num_devices; i++) {
      const char *old_path = old_device_paths[i];
      const char *new_path = temp_config.keyboard_devices[i];
      if ((old_path == NULL) != (new_path == NULL) ||
          (old_path != NULL && new_path != NULL &&
           strcmp(old_path, new_path) != 0)) {
        devices_changed = true;
        break;
      }
    }
  }

  // Free saved paths
  if (old_device_paths != NULL) {
    for (int i = 0; i < old_num_devices; i++) {
      free(old_device_paths[i]);
    }
    free(old_device_paths);
  }

  // Swap in new config under animation lock to avoid reader races
  pthread_mutex_lock(&anim_lock);
  config_cleanup_full(&g_config);
  g_config = temp_config;

  if (g_forced_monitor_name) {
    bongocat_error_t force_result =
        config_apply_forced_monitor(&g_config, g_forced_monitor_name);
    if (force_result != BONGOCAT_SUCCESS) {
      bongocat_log_warning("Failed to keep forced monitor '%s' during reload",
                           g_forced_monitor_name);
    }
  }
  pthread_mutex_unlock(&anim_lock);

  // Update the running systems with new config
  wayland_update_config(&g_config);

  // Check if input devices changed and restart monitoring if needed
  if (devices_changed) {
    bongocat_log_info("Input devices changed, restarting input monitoring");
    bongocat_error_t input_result = input_restart_monitoring(
        g_config.keyboard_devices, g_config.num_keyboard_devices,
        g_config.keyboard_names, g_config.num_names,
        g_config.hotplug_scan_interval, g_config.enable_debug);
    if (input_result != BONGOCAT_SUCCESS) {
      bongocat_log_error("Failed to restart input monitoring: %s",
                         bongocat_error_string(input_result));
    } else {
      bongocat_log_info("Input monitoring restarted successfully");
    }
  }

  bongocat_log_info("Configuration reloaded successfully!");
  bongocat_log_info("New screen dimensions: %dx%d", g_config.screen_width,
                    g_config.bar_height);
}

static void config_reload_callback(const char *config_path) {
  (void)config_path;
  atomic_store(&g_reload_pending, true);
}

static void config_process_pending_reload(void) {
  if (!atomic_exchange(&g_reload_pending, false)) {
    return;
  }

  const char *config_path =
      (g_config_watcher.config_path && g_config_watcher.config_path[0] != '\0')
          ? g_config_watcher.config_path
          : "bongocat.conf";
  config_reload_apply(config_path);
}

static void wayland_tick_callback(void) {
  config_process_pending_reload();
}

static bongocat_error_t config_setup_watcher(const char *config_file) {
  const char *watch_path = config_file ? config_file : "bongocat.conf";

  if (config_watcher_init(&g_config_watcher, watch_path,
                          config_reload_callback) == 0) {
    config_watcher_start(&g_config_watcher);
    bongocat_log_info("Config file watching enabled for: %s", watch_path);
    return BONGOCAT_SUCCESS;
  } else {
    bongocat_log_warning(
        "Failed to initialize config watcher, continuing without hot-reload");
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
    bongocat_log_error("Failed to initialize Wayland: %s",
                       bongocat_error_string(result));
    return result;
  }

  // Initialize animation system
  result = animation_init(&g_config);
  if (result != BONGOCAT_SUCCESS) {
    bongocat_log_error("Failed to initialize animation system: %s",
                       bongocat_error_string(result));
    return result;
  }

  // Start input monitoring
  result = input_start_monitoring(
      g_config.keyboard_devices, g_config.num_keyboard_devices,
      g_config.keyboard_names, g_config.num_names,
      g_config.hotplug_scan_interval, g_config.enable_debug);
  if (result != BONGOCAT_SUCCESS) {
    bongocat_log_error("Failed to start input monitoring: %s",
                       bongocat_error_string(result));
    return result;
  }

  // Start animation thread
  result = animation_start();
  if (result != BONGOCAT_SUCCESS) {
    bongocat_log_error("Failed to start animation thread: %s",
                       bongocat_error_string(result));
    return result;
  }

  return BONGOCAT_SUCCESS;
}

static void system_cleanup_and_exit(int exit_code) {
  bongocat_log_info("Performing cleanup...");

  // Remove PID file
  if (g_manage_pid_file) {
    process_remove_pid_file();
  }

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
// COMMAND LINE PROCESSING MODULE
// =============================================================================

static void cli_show_help(const char *program_name) {
  printf("Bongo Cat Wayland Overlay\n");
  printf("Usage: %s [options]\n", program_name);
  printf("Options:\n");
  printf("  -h, --help            Show this help message\n");
  printf("  -v, --version         Show version information\n");
  printf(
      "  -c, --config          Specify config file (default: auto-detect)\n");
  printf("  -w, --watch-config    Watch config file for changes and reload "
         "automatically\n");
  printf("  -t, --toggle          Toggle bongocat on/off (start if not "
         "running, stop if running)\n");
  printf("  -m, --monitor NAME    Bind to a specific monitor output\n");
  printf("\nConfiguration search order:\n");
  printf("  1. $XDG_CONFIG_HOME/bongocat/bongocat.conf\n");
  printf("  2. ~/.config/bongocat/bongocat.conf\n");
  printf("  3. ./bongocat.conf\n");
  printf("\nMulti-monitor: set monitor=OUT1,OUT2 in config to show on "
         "multiple monitors.\n");
}

static void cli_show_version(void) {
  printf("Bongo Cat Overlay v" BONGOCAT_VERSION "\n");
  printf("Built with fast optimizations\n");
}

static int cli_parse_arguments(int argc, char *argv[], cli_args_t *args) {
  // Initialize arguments with defaults
  *args = (cli_args_t){.config_file = NULL,
                       .monitor_name = NULL,
                       .multi_monitor_child = false,
                       .watch_config = false,
                       .toggle_mode = false,
                       .show_help = false,
                       .show_version = false};

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      args->show_help = true;
    } else if (strcmp(argv[i], "--version") == 0 ||
               strcmp(argv[i], "-v") == 0) {
      args->show_version = true;
    } else if (strcmp(argv[i], "--config") == 0 || strcmp(argv[i], "-c") == 0) {
      if (i + 1 < argc) {
        args->config_file = argv[i + 1];
        i++;  // Skip the next argument since it's the config file path
      } else {
        bongocat_log_error("--config option requires a file path");
        return 1;
      }
    } else if (strcmp(argv[i], "--watch-config") == 0 ||
               strcmp(argv[i], "-w") == 0) {
      args->watch_config = true;
    } else if (strcmp(argv[i], "--toggle") == 0 || strcmp(argv[i], "-t") == 0) {
      args->toggle_mode = true;
    } else if (strcmp(argv[i], "--monitor") == 0 ||
               strcmp(argv[i], "-m") == 0) {
      if (i + 1 < argc) {
        args->monitor_name = argv[i + 1];
        i++;
      } else {
        bongocat_log_error("--monitor option requires an output name");
        return 1;
      }
    } else if (strcmp(argv[i], "--multi-monitor-child") == 0) {
      args->multi_monitor_child = true;
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
  bongocat_error_init(1);  // Enable debug initially

  bongocat_log_info("Starting Bongo Cat Overlay v" BONGOCAT_VERSION);

  // Parse command line arguments
  cli_args_t args;
  if (cli_parse_arguments(argc, argv, &args) != 0) {
    return 1;
  }

  g_manage_pid_file = !args.multi_monitor_child;
  g_forced_monitor_name = args.monitor_name;

  if (args.multi_monitor_child && !args.monitor_name) {
    bongocat_log_error("--multi-monitor-child requires --monitor");
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
  if (args.toggle_mode && g_manage_pid_file) {
    int toggle_result = process_handle_toggle();
    if (toggle_result >= 0) {
      return toggle_result;  // Either successfully toggled off or error
    }
    // toggle_result == -1 means continue with startup
  } else if (args.toggle_mode) {
    bongocat_log_error(
        "--toggle is not valid in internal multi-monitor child mode");
    return 1;
  }

  // Setup signal handlers
  result = signal_setup_handlers();
  if (result != BONGOCAT_SUCCESS) {
    bongocat_log_error("Failed to setup signal handlers: %s",
                       bongocat_error_string(result));
    return 1;
  }

  // Create PID file to track this instance
  if (g_manage_pid_file) {
    int pid_fd = process_create_pid_file();
    if (pid_fd == -2) {
      bongocat_log_error("Another instance of bongocat is already running");
      return 1;
    } else if (pid_fd < 0) {
      bongocat_log_error("Failed to create PID file");
      return 1;
    }
    (void)pid_fd;
  }

  // Resolve and load configuration
  char *resolved_config = config_resolve_path(args.config_file);
  result = load_config(&g_config, resolved_config);
  if (result != BONGOCAT_SUCCESS) {
    bongocat_log_error("Failed to load configuration: %s",
                       bongocat_error_string(result));
    if (g_manage_pid_file) {
      process_remove_pid_file();
    }
    free(resolved_config);
    return 1;
  }

  bongocat_log_info("Screen dimensions: %dx%d", g_config.screen_width,
                    g_config.bar_height);

  if (g_config.enable_debug) {
    bongocat_log_warning(
        "DEBUG MODE ENABLED: Keystrokes are being logged "
        "to stdout/stderr. Disable in config if not intended.");
  }

  // Handle multi-monitor mode
  if (g_forced_monitor_name) {
    // Child process: override output_name with assigned monitor
    if (config_apply_forced_monitor(&g_config, g_forced_monitor_name) !=
        BONGOCAT_SUCCESS) {
      bongocat_log_error("Failed to apply forced monitor '%s'",
                         g_forced_monitor_name);
      free(resolved_config);
      return 1;
    }
  } else if (g_config.num_output_names > 1) {
    // Parent process: launch one child per configured monitor
    bongocat_log_info("Multi-monitor mode enabled with %d configured monitors",
                      g_config.num_output_names);

    int mm_result =
        multi_monitor_launch(argc, argv, resolved_config, args.watch_config,
                             g_config.output_names, g_config.num_output_names);

    if (mm_result == -1) {
      // Single monitor after config filtering, fall through
      bongocat_log_info("Falling back to single-monitor mode");
    } else {
      free(resolved_config);
      config_cleanup_full(&g_config);
      config_cleanup();
      return mm_result;
    }
  }

  // Initialize config watcher if requested
  if (args.watch_config) {
    config_setup_watcher(resolved_config);
  }
  free(resolved_config);

  // Initialize all system components
  result = system_initialize_components();
  if (result != BONGOCAT_SUCCESS) {
    system_cleanup_and_exit(1);
  }

  bongocat_log_info("Bongo Cat Overlay started successfully");

  // Main Wayland event loop with graceful shutdown
  wayland_set_tick_callback(wayland_tick_callback);
  result = wayland_run(&running);
  if (result != BONGOCAT_SUCCESS) {
    bongocat_log_error("Wayland event loop error: %s",
                       bongocat_error_string(result));
    system_cleanup_and_exit(1);
  }

  bongocat_log_info("Main loop exited, shutting down");
  system_cleanup_and_exit(0);

  return 0;  // Never reached
}
