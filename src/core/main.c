#define _GNU_SOURCE
#include "config/config.h"
#include "config/config_watcher.h"
#include "core/bongocat.h"
#include "core/control.h"
#include "graphics/animation.h"
#include "platform/input.h"
#include "platform/niri.h"
#include "platform/wayland.h"
#include "utils/clock.h"
#include "utils/error.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/poll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define INPUT_RETRY_US 5000000  // Minimum interval between helper starts
#define TOGGLE_WAIT_US 2000000  // How long --toggle waits for a stopping cat
#define TOGGLE_POLL_US 50000

static volatile sig_atomic_t running = 1;
static int signal_fd = -1;
static int sleep_fd = -1;
static config_t config;
static config_watcher_t watcher;  // Inactive unless --watch-config
static char *config_path;         // NULL while running on defaults
static const char *monitor_override;
static bool hidden, paused;
static int64_t input_retry_at;

// =============================================================================
// SIGNALS, SCHEDULE AND INPUT
// =============================================================================

static void stop_signal(int signal) {
  (void)signal;
  int saved = errno;
  running = 0;
  uint64_t value = 1;
  if (write(signal_fd, &value, sizeof(value)) < 0) {
    // The flag alone still ends the loop at its next wakeup.
  }
  errno = saved;
}

static bool setup_signals(void) {
  signal_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (signal_fd < 0) {
    return false;
  }
  struct sigaction action = {.sa_handler = stop_signal};
  sigemptyset(&action.sa_mask);
  const int signals[] = {SIGTERM, SIGINT, SIGQUIT, SIGHUP};
  for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
    if (sigaction(signals[i], &action, NULL) < 0) {
      return false;
    }
  }
  signal(SIGPIPE, SIG_IGN);
  return true;
}

// Arms a wall-clock timer for the next sleep_begin/sleep_end. Wall-clock
// timers also fire after suspend, and clock changes cancel them.
static void update_sleep_schedule(void) {
  // time() reads a coarse clock that can lag the timerfd that just fired,
  // which would re-arm the same, already passed boundary.
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  time_t next;
  wayland_set_sleeping(sleep_schedule(&config, now.tv_sec, &next));
  struct itimerspec spec = {.it_value = {.tv_sec = next}};
  if (sleep_fd >= 0 &&
      timerfd_settime(sleep_fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET,
                      &spec, NULL) < 0) {
    bongocat_log_warning("Cannot schedule sleep: %s", strerror(errno));
  }
}

static void start_input(void) {
  input_retry_at = monotonic_us() + INPUT_RETRY_US;
  if (input_start(&config) != BONGOCAT_SUCCESS) {
    bongocat_log_warning("Input helper unavailable; retrying in 5s");
  }
}

// =============================================================================
// CONFIGURATION
// =============================================================================

static bool same_strings(char **a, int count_a, char **b, int count_b) {
  if (count_a != count_b) {
    return false;
  }
  for (int i = 0; i < count_a; i++) {
    if (strcmp(a[i], b[i]) != 0) {
      return false;
    }
  }
  return true;
}

// --monitor replaces the configured output list.
static bongocat_error_t force_monitor(config_t *settings) {
  if (!monitor_override) {
    return BONGOCAT_SUCCESS;
  }
  char *name = strdup(monitor_override);
  char **names = name ? malloc(sizeof(*names)) : NULL;
  if (!names) {
    free(name);
    return BONGOCAT_ERROR_MEMORY;
  }
  for (int i = 0; i < settings->num_monitors; i++) {
    free(settings->monitors[i]);
  }
  free((void *)settings->monitors);
  names[0] = name;
  settings->monitors = names;
  settings->num_monitors = 1;
  return BONGOCAT_SUCCESS;
}

static int reload(void) {
  config_t next;
  if (config_load(&next, config_path, true) != BONGOCAT_SUCCESS ||
      force_monitor(&next) != BONGOCAT_SUCCESS) {
    config_free(&next);
    bongocat_log_set_debug(config.enable_debug);
    bongocat_log_warning("Reload rejected; keeping current configuration");
    return 1;
  }
  bool input_changed =
      !same_strings(next.keyboard_devices, next.num_keyboard_devices,
                    config.keyboard_devices, config.num_keyboard_devices) ||
      !same_strings(next.keyboard_names, next.num_keyboard_names,
                    config.keyboard_names, config.num_keyboard_names);
  config_t old = config;
  config = next;
  wayland_update_config(&config);
  update_sleep_schedule();
  if (input_changed) {
    input_stop();
    start_input();
  }
  config_free(&old);
  bongocat_log_info("Configuration reloaded");
  return 0;
}

// =============================================================================
// CONTROL REQUESTS
// =============================================================================

static int command(const char *request, char *reply, size_t size) {
  int result = 0;
  if (strcmp(request, "stop") == 0) {
    running = 0;
  } else if (strcmp(request, "hide") == 0 || strcmp(request, "show") == 0) {
    hidden = strcmp(request, "hide") == 0;
    wayland_set_hidden(hidden);
  } else if (strcmp(request, "pause") == 0 || strcmp(request, "resume") == 0) {
    paused = strcmp(request, "pause") == 0;
    wayland_set_paused(paused);
  } else if (strcmp(request, "reload") == 0) {
    result = reload();
  } else if (strcmp(request, "status") == 0) {
    snprintf(reply, size,
             "running pid=%ld hidden=%s paused=%s input=%s devices=%u "
             "config=%s",
             (long)getpid(), hidden ? "yes" : "no", paused ? "yes" : "no",
             input_running() ? "connected" : "restarting", input_device_count(),
             config_path ? config_path : "(defaults)");
    return 0;
  } else {
    result = 1;
  }
  snprintf(reply, size, "%s", result ? "request failed" : "ok");
  return result;
}

// =============================================================================
// EVENT LOOP
// =============================================================================

enum {
  POLL_WAYLAND,
  POLL_SIGNAL,
  POLL_INPUT,
  POLL_WATCHER,
  POLL_NIRI,
  POLL_SLEEP,
  POLL_CONTROL,  // Listening socket, then clients
};
#define POLL_COUNT (POLL_CONTROL + 1 + CONTROL_MAX_CLIENTS)

static void earliest(int64_t *deadline, int64_t candidate) {
  if (candidate && (!*deadline || candidate < *deadline)) {
    *deadline = candidate;
  }
}

static bool due(int64_t deadline) {
  return deadline && monotonic_us() >= deadline;
}

static void drain(int fd) {
  uint64_t value;
  while (read(fd, &value, sizeof(value)) > 0) {}
}

static bongocat_error_t run(void) {
  struct pollfd fds[POLL_COUNT];
  while (running) {
    if (!input_running() && due(input_retry_at)) {
      start_input();
    }
    int64_t deadline;
    bongocat_error_t result = wayland_prepare(&fds[POLL_WAYLAND], &deadline);
    if (result != BONGOCAT_SUCCESS) {
      return result;
    }
    earliest(&deadline, config_watcher_deadline(&watcher));
    earliest(&deadline, control_deadline());
    if (!input_running()) {
      earliest(&deadline, input_retry_at);
    }
    fds[POLL_SIGNAL] = (struct pollfd){.fd = signal_fd, .events = POLLIN};
    fds[POLL_INPUT] = (struct pollfd){.fd = input_fd(), .events = POLLIN};
    fds[POLL_WATCHER] =
        (struct pollfd){.fd = config_watcher_fd(&watcher), .events = POLLIN};
    fds[POLL_NIRI] = (struct pollfd){.fd = niri_fd(), .events = POLLIN};
    fds[POLL_SLEEP] = (struct pollfd){.fd = sleep_fd, .events = POLLIN};
    int control[1 + CONTROL_MAX_CLIENTS];
    size_t controls = control_fds(control, 1 + CONTROL_MAX_CLIENTS);
    for (size_t i = 0; i < controls; i++) {
      fds[POLL_CONTROL + i] =
          (struct pollfd){.fd = control[i], .events = POLLIN};
    }
    nfds_t count = POLL_CONTROL + controls;
    bool failed = false;
    if (poll(fds, count, poll_timeout_until(deadline, monotonic_us())) < 0) {
      failed = errno != EINTR;
      if (failed) {
        bongocat_log_error("poll: %s", strerror(errno));
      }
      for (nfds_t i = 0; i < count; i++) {
        fds[i].revents = 0;
      }
    }
    // Always pairs with the read prepared by wayland_prepare().
    result = wayland_dispatch(&fds[POLL_WAYLAND]);
    if (failed || result != BONGOCAT_SUCCESS) {
      return failed ? BONGOCAT_ERROR_WAYLAND : result;
    }
    if (fds[POLL_SIGNAL].revents) {
      drain(signal_fd);
    }
    if (fds[POLL_INPUT].revents) {
      int64_t time = 0;
      unsigned paws = input_read(&time);
      wayland_add_paws(paws, time);
    }
    if ((fds[POLL_WATCHER].revents || due(config_watcher_deadline(&watcher))) &&
        config_watcher_process(&watcher)) {
      reload();
    }
    if (fds[POLL_NIRI].revents) {
      niri_poll();
    }
    if (fds[POLL_SLEEP].revents) {
      drain(sleep_fd);
      update_sleep_schedule();
    }
    bool control_ready = due(control_deadline());
    for (size_t i = 0; i < controls; i++) {
      control_ready |= fds[POLL_CONTROL + i].revents != 0;
    }
    if (control_ready) {
      control_process(command);
    }
  }
  return BONGOCAT_SUCCESS;
}

// Waits for an instance that is shutting down to release the lock.
static int lock_instance(bool wait) {
  int result = instance_lock();
  for (int64_t until = monotonic_us() + TOGGLE_WAIT_US;
       wait && result == INSTANCE_LOCKED && monotonic_us() < until;
       result = instance_lock()) {
    struct timespec delay = {.tv_nsec = (long)TOGGLE_POLL_US * 1000};
    nanosleep(&delay, NULL);
  }
  return result;
}

static bool start_application(bool watch, bool toggle) {
  if (config_load(&config, config_path, false) != BONGOCAT_SUCCESS ||
      force_monitor(&config) != BONGOCAT_SUCCESS) {
    return false;
  }
  int lock = lock_instance(toggle);
  if (lock == INSTANCE_LOCKED) {
    bongocat_log_error("bongocat is already running");
    return false;
  }
  if (lock < 0 || control_start() < 0) {
    bongocat_log_error("Cannot create runtime files: %s", strerror(errno));
    return false;
  }
  sleep_fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC);
  if (sleep_fd < 0 || !setup_signals()) {
    bongocat_log_error("Cannot set up signals and timers: %s", strerror(errno));
    return false;
  }
  if (watch && !config_path) {
    bongocat_log_info("No configuration file to watch");
  } else if (watch && !config_watcher_init(&watcher, config_path)) {
    bongocat_log_warning("Cannot watch %s; use --reload", config_path);
  }
  if (animation_init() != BONGOCAT_SUCCESS ||
      wayland_init(&config) != BONGOCAT_SUCCESS) {
    return false;
  }
  update_sleep_schedule();
  start_input();
  return true;
}

static void stop_application(void) {
  control_cleanup();
  input_stop();
  niri_cleanup();
  wayland_cleanup();
  animation_cleanup();
  config_watcher_cleanup(&watcher);
  if (signal_fd >= 0) {
    close(signal_fd);
  }
  if (sleep_fd >= 0) {
    close(sleep_fd);
  }
  signal_fd = sleep_fd = -1;
  config_free(&config);
  instance_unlock();
}

static int run_application(bool watch, bool toggle) {
  bongocat_error_t result = BONGOCAT_ERROR_CONFIG;
  if (start_application(watch, toggle)) {
    result = run();
    if (result != BONGOCAT_SUCCESS) {
      bongocat_log_error("Runtime stopped: %s", bongocat_error_string(result));
    }
  }
  stop_application();
  return result == BONGOCAT_SUCCESS ? 0 : 1;
}

// =============================================================================
// COMMAND LINE
// =============================================================================

static void help(const char *program) {
  printf("Usage: %s [options]\n"
         "  -c, --config FILE    Configuration path (XDG search by default)\n"
         "  -w, --watch-config   Reload 300 ms after config changes settle\n"
         "  -m, --monitor NAME   Override configured output selection\n"
         "  -t, --toggle         Start or stop the running application\n"
         "  --hide, --show       Control visibility of every overlay\n"
         "  --pause, --resume    Display idle frame or resume input animation\n"
         "  --reload, --status   Reload config or query running application\n"
         "  --check-config       Strictly validate the configuration\n"
         "  --list-devices       List input devices and keyboard settings\n"
         "  --list-monitors      List Wayland outputs, sizes and scales\n"
         "  --doctor             Check config, devices and protocols\n"
         "  -h, --help           Show help\n"
         "  -v, --version        Show version\n",
         program);
}

int main(int argc, char **argv) {
  input_privilege_init();
  if (argc > 1 && strcmp(argv[1], "--input-helper") == 0) {
    return input_helper_main(argc, argv);
  }
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
    if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
      help(argv[0]);
      return 0;
    }
    if (!strcmp(arg, "-v") || !strcmp(arg, "--version")) {
      printf("%s\n", BONGOCAT_VERSION);
      return 0;
    }
    bool config_option = !strcmp(arg, "-c") || !strcmp(arg, "--config");
    bool monitor_option = !strcmp(arg, "-m") || !strcmp(arg, "--monitor");
    if (config_option || monitor_option) {
      if (++i >= argc) {
        fprintf(stderr, "%s requires a value\n", arg);
        return 1;
      }
      if (config_option) {
        explicit_path = argv[i];
      } else {
        monitor_override = argv[i];
      }
    } else if (!strcmp(arg, "-w") || !strcmp(arg, "--watch-config")) {
      watch = true;
    } else if (!strcmp(arg, "-t") || !strcmp(arg, "--toggle")) {
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
      request = arg + 2;
    } else {
      fprintf(stderr, "Unknown option: %s\n", arg);
      return 1;
    }
  }
  // Listing opens devices, so it runs before the input group is dropped.
  int device_failure = devices || doctor ? input_list_devices() : 0;
  input_privilege_drop();
  if (request) {
    return control_request(request) == 0 ? 0 : 1;
  }
  if (devices && !doctor) {
    return device_failure;
  }
  if (monitors && !doctor) {
    return wayland_list_monitors(false);
  }
  config_path = config_resolve_path(explicit_path);
  if (explicit_path && !config_path) {
    return 1;
  }
  if (check || doctor) {
    config_t checked;
    bool valid = config_load(&checked, config_path, true) == BONGOCAT_SUCCESS;
    config_free(&checked);
    printf("Config: %s (%s)\n",
           config_path ? config_path : "no file, using defaults",
           valid ? "valid" : "invalid");
    int failure = !valid;
    if (doctor) {
      failure |= device_failure | wayland_list_monitors(true);
    }
    free(config_path);
    return failure;
  }
  if (toggle) {
    int result = control_request("stop");
    if (result != 2) {
      free(config_path);
      return result;
    }
  }
  int exit_code = run_application(watch, toggle);
  free(config_path);
  return exit_code;
}
