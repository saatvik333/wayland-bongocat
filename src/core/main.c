#define _GNU_SOURCE
#include "config/config.h"
#include "core/bongocat.h"
#include "core/control.h"
#include "graphics/animation.h"
#include "platform/hyprland.h"
#include "platform/input.h"
#include "platform/wayland.h"
#include "utils/error.h"

#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;
static int signal_fd = -1;
static config_t config;
static ConfigWatcher watcher = {.inotify_fd = -1, .watch_fd = -1};
static char *config_path;
static const char *monitor_override;
static bool hidden, paused;
static bool reload_pending;
static int64_t input_retry_at;
static int64_t monotonic_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((int64_t)ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

static void stop_signal(int signal) {
  (void)signal;
  int saved = errno;
  running = 0;
  uint64_t value = 1;
  if (signal_fd >= 0) {
    ssize_t result = write(signal_fd, &value, sizeof(value));
    (void)result;
  }
  errno = saved;
}
static int setup_signals(void) {
  signal_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (signal_fd < 0) {
    return -1;
  }
  struct sigaction action = {.sa_handler = stop_signal};
  sigemptyset(&action.sa_mask);
  int signals[] = {SIGTERM, SIGINT, SIGQUIT, SIGHUP};
  for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
    if (sigaction(signals[i], &action, NULL) < 0) {
      return -1;
    }
  }
  signal(SIGPIPE, SIG_IGN);
  return 0;
}
static bool arrays_equal(char **a, int na, char **b, int nb) {
  if (na != nb) {
    return false;
  }
  for (int i = 0; i < na; i++) {
    if (strcmp(a[i], b[i]) != 0) {
      return false;
    }
  }
  return true;
}
static bongocat_error_t force_monitor(config_t *settings) {
  if (!monitor_override) {
    return BONGOCAT_SUCCESS;
  }
  char *name = strdup(monitor_override);
  if (!name) {
    return BONGOCAT_ERROR_MEMORY;
  }
  free(settings->output_name);
  settings->output_name = name;
  for (int i = 0; i < settings->num_output_names; i++) {
    free(settings->output_names[i]);
  }
  free((void *)settings->output_names);
  settings->output_names = NULL;
  settings->num_output_names = 0;
  return BONGOCAT_SUCCESS;
}
static int reload(void) {
  config_t next = {0};
  bongocat_error_t result = load_config_strict(&next, config_path);
  if (result == BONGOCAT_SUCCESS) {
    result = force_monitor(&next);
  }
  if (result != BONGOCAT_SUCCESS) {
    config_cleanup_full(&next);
    bongocat_error_init(config.enable_debug);
    bongocat_log_warning("Reload rejected; keeping current configuration");
    return 1;
  }
  bool input_changed =
      (next.hotplug_scan_interval != config.hotplug_scan_interval ||
       !arrays_equal(next.keyboard_devices, next.num_keyboard_devices,
                     config.keyboard_devices, config.num_keyboard_devices) ||
       !arrays_equal(next.keyboard_names, next.num_names, config.keyboard_names,
                     config.num_names)) != 0;
  config_t old = config;
  config = next;
  wayland_update_config(&config);
  if (input_changed) {
    result = input_restart_monitoring(config.keyboard_devices,
                                      config.num_keyboard_devices,
                                      config.keyboard_names, config.num_names,
                                      config.hotplug_scan_interval, 0);
    if (result != BONGOCAT_SUCCESS) {
      bongocat_log_warning("Input helper restart failed; retrying");
    }
  }
  config_cleanup_full(&old);
  bongocat_error_init(config.enable_debug);
  return 0;
}
static void changed(const char *path) {
  (void)path;
  reload_pending = true;
}
static int command(const char *request, char *response, size_t capacity) {
  int result = 0;
  if (strcmp(request, "stop") == 0) {
    {
      running = 0;
    }
  } else if (strcmp(request, "hide") == 0) {
    hidden = true;
    wayland_set_hidden(true);
  } else if (strcmp(request, "show") == 0) {
    hidden = false;
    wayland_set_hidden(false);
  } else if (strcmp(request, "pause") == 0 || strcmp(request, "resume") == 0) {
    paused = strcmp(request, "pause") == 0;
    input_process_events();
    if (pending_paws) {
      atomic_store(pending_paws, 0);
    }
    animation_set_paused(paused);
    wayland_request_redraw();
  } else if (strcmp(request, "reload") == 0) {
    { result = reload(); }
  } else if (strcmp(request, "status") == 0) {
    snprintf(
        response, capacity,
        "running pid=%ld hidden=%s paused=%s input=%s devices=%u config=%s",
        (long)getpid(), (int)hidden ? "yes" : "no", (int)paused ? "yes" : "no",
        (int)input_child_is_alive() ? "connected" : "restarting",
        input_device_count(), config_path);
    return 0;
  } else {
    { result = 1; }
  }
  snprintf(response, capacity, "%s", result ? "request failed" : "ok");
  return result;
}
static void tick(void) {
  hypr_poll();
  config_watcher_process(&watcher);
  if (reload_pending) {
    reload_pending = false;
    reload();
  }
  control_process(command);
  if (!input_child_is_alive() && monotonic_ms() >= input_retry_at) {
    input_retry_at = monotonic_ms() + 5000;
    bongocat_error_t result = input_restart_monitoring(
        config.keyboard_devices, config.num_keyboard_devices,
        config.keyboard_names, config.num_names, config.hotplug_scan_interval,
        0);
    if (result != BONGOCAT_SUCCESS) {
      bongocat_log_warning("Input helper unavailable; retrying in 5s");
    }
  }
}
static int runtime_timeout(void) {
  int candidates[] = {config_watcher_timeout(&watcher), control_timeout(),
                      hypr_timeout(), -1};
  if (!input_child_is_alive()) {
    int64_t remaining = input_retry_at - monotonic_ms();
    candidates[3] = remaining > 0 ? (int)remaining : 0;
  }
  int timeout = -1;
  for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
    if (candidates[i] >= 0 && (timeout < 0 || candidates[i] < timeout)) {
      timeout = candidates[i];
    }
  }
  return timeout;
}
static int runtime_fds(int *fds, size_t capacity) {
  size_t count = 0;
  int basic[] = {signal_fd, input_get_wake_fd(), watcher.inotify_fd,
                 hypr_poll_fd()};
  for (size_t i = 0; i < sizeof(basic) / sizeof(basic[0]) && count < capacity;
       i++) {
    if (basic[i] >= 0) {
      fds[count++] = basic[i];
    }
  }
  return (int)count + control_fds(fds + count, capacity - count);
}
static void help(const char *program) {
  printf("Usage: %s [options]\n"
         "  -c, --config FILE    Configuration path (XDG search by default)\n"
         "  -w, --watch-config   Reload 300 ms after config changes settle\n"
         "  -m, --monitor NAME   Override configured output selection\n"
         "  -t, --toggle         Start or stop the running application\n"
         "  --hide, --show       Control visibility of every overlay\n"
         "  --pause, --resume    Display idle frame or resume input animation\n"
         "  --reload, --status   Reload config or query running application\n"
         "  --check-config       Strict validation without Wayland or input "
         "access\n"
         "  --list-devices       List evdev devices and keyboard capabilities\n"
         "  --list-monitors      List Wayland outputs, dimensions and scales\n"
         "  --doctor             Check config, protocols, devices and "
         "permissions\n"
         "  -h, --help           Show help\n"
         "  -v, --version        Show version\n",
         program);
}
static int run_application(bool watch, bongocat_error_t result) {
  int exit_code = 1;
  if (result != BONGOCAT_SUCCESS ||
      force_monitor(&config) != BONGOCAT_SUCCESS) {
    goto cleanup;
  }
  if (instance_lock() < 0) {
    bongocat_log_error("Cannot lock instance: %s", strerror(errno));
    goto cleanup;
  }
  if (control_start() < 0 || setup_signals() < 0) {
    goto cleanup;
  }
  if (watch && config_watcher_init(&watcher, config_path, changed) == 0) {
    config_watcher_start(&watcher);
  }
  result = animation_init(&config);
  if (result != BONGOCAT_SUCCESS) {
    goto cleanup;
  }
  result = wayland_init(&config);
  if (result != BONGOCAT_SUCCESS) {
    goto cleanup;
  }
  result = input_start_monitoring(
      config.keyboard_devices, config.num_keyboard_devices,
      config.keyboard_names, config.num_names, config.hotplug_scan_interval, 0);
  if (result != BONGOCAT_SUCCESS) {
    goto cleanup;
  }
  wayland_set_tick_callback(tick);
  wayland_set_runtime_fds(runtime_fds);
  wayland_set_runtime_timeout(runtime_timeout);
  result = wayland_run(&running);
  if (result != BONGOCAT_SUCCESS) {
    bongocat_log_error("Runtime stopped: %s", bongocat_error_string(result));
  }
  exit_code = result == BONGOCAT_SUCCESS ? 0 : 1;
cleanup:
  config_watcher_cleanup(&watcher);
  hypr_cleanup();
  input_cleanup();
  wayland_cleanup();
  animation_cleanup();
  control_cleanup();
  if (signal_fd >= 0) {
    int fd = signal_fd;
    signal_fd = -1;
    close(fd);
  }
  config_cleanup_full(&config);
  free(config_path);
  instance_unlock();
  return exit_code;
}

int main(int argc, char **argv) {
  input_privilege_init();
  if (argc > 1 && strcmp(argv[1], "--input-helper") == 0) {
    return input_helper_main(argc, argv);
  }
  input_privilege_drop();
  bongocat_error_init(0);
  const char *explicit_path = NULL;
  const char *request = NULL;
  bool watch = false;
  bool toggle = false;
  bool check = false;
  bool devices = false;
  bool monitors = false;
  bool doctor = false;
  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) {
      help(argv[0]);
      return 0;
    }
    if (!strcmp(arg, "--version") || !strcmp(arg, "-v")) {
      puts(BONGOCAT_VERSION);
      return 0;
    }
    if (!strcmp(arg, "--config") || !strcmp(arg, "-c") ||
        !strcmp(arg, "--monitor") || !strcmp(arg, "-m")) {
      if (++i >= argc) {
        fprintf(stderr, "%s requires a value\n", arg);
        return 1;
      }
      if (!strcmp(arg, "--config") || !strcmp(arg, "-c")) {
        explicit_path = argv[i];
      } else {
        monitor_override = argv[i];
      }
    } else if (!strcmp(arg, "--watch-config") || !strcmp(arg, "-w")) {
      watch = true;
    } else if (!strcmp(arg, "--toggle") || !strcmp(arg, "-t")) {
      toggle = true;
    } else if (!strcmp(arg, "--check-config")) {
      check = true;
    } else if (!strcmp(arg, "--list-devices")) {
      devices = true;
    } else if (!strcmp(arg, "--list-monitors")) {
      monitors = true;
    } else if (!strcmp(arg, "--doctor")) {
      doctor = true;
    } else if (!strcmp(arg, "--hide") || !strcmp(arg, "--show") ||
               !strcmp(arg, "--pause") || !strcmp(arg, "--resume") ||
               !strcmp(arg, "--reload") || !strcmp(arg, "--status")) {
      if (request) {
        fprintf(stderr, "Select one control command\n");
        return 1;
      }
      request = arg + 2;
    } else {
      fprintf(stderr, "Unknown option: %s\n", arg);
      return 1;
    }
  }
  if (request) {
    return control_request(request) == 0 ? 0 : 1;
  }
  if (toggle) {
    int result = control_request("stop");
    if (result != 2) {
      return result;
    }
  }
  if (devices && !doctor) {
    return input_list_devices();
  }
  if (monitors && !doctor) {
    return wayland_list_monitors(false);
  }
  config_path = config_resolve_path(explicit_path);
  if (!config_path) {
    config_path = strdup("bongocat.conf");
  }
  if (!config_path) {
    return 1;
  }
  bongocat_error_t result = (check || doctor)
                                ? load_config_strict(&config, config_path)
                                : load_config(&config, config_path);
  if (check || doctor) {
    printf("Config: %s (%s)\n", config_path,
           result == BONGOCAT_SUCCESS ? "valid" : "invalid");
    int failure = result != BONGOCAT_SUCCESS;
    if (doctor) {
      failure |= input_list_devices();
      failure |= wayland_list_monitors(true);
    }
    config_cleanup_full(&config);
    free(config_path);
    return failure;
  }
  return run_application(watch, result);
}
