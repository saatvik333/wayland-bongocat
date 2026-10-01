#define _POSIX_C_SOURCE 200809L
#include "config/config.h"

#include "core/bongocat.h"
#include "utils/error.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// =============================================================================
// CONFIGURATION CONSTANTS AND VALIDATION RANGES
// =============================================================================

#define MIN_CAT_HEIGHT     10
#define MAX_CAT_HEIGHT     200
#define MIN_OVERLAY_HEIGHT 20
#define MAX_OVERLAY_HEIGHT 300
#define MIN_FPS            1
#define MAX_FPS            120
#define MIN_DURATION       10
#define MAX_DURATION       5000
#define MAX_INTERVAL       3600

// =============================================================================
// CONFIGURATION VALIDATION MODULE
// =============================================================================

static bool strict_parse;
static bool validation_failed;
static config_diagnostic_callback_t diagnostic_callback;
static void *diagnostic_data;
static char diagnostic_path[PATH_MAX];
static int diagnostic_line;
static void diagnostic(const char *key, const char *message) {
  config_diagnostic_t item = {.file = diagnostic_path,
                              .line = diagnostic_line,
                              .key = key,
                              .severity = (int)strict_parse ? CONFIG_ERROR
                                                            : CONFIG_WARNING,
                              .message = message};
  if (diagnostic_callback) {
    diagnostic_callback(&item, diagnostic_data);
  }
  bongocat_log_warning("%s:%d: %s: key '%s': %s", item.file, item.line,
                       (int)strict_parse ? "error" : "warning", key ? key : "",
                       message);
}

static void config_clamp_int(int *value, int min, int max, const char *name) {
  if (*value < min || *value > max) {
    validation_failed = true;
    bongocat_log_warning("%s %d out of range [%d-%d], clamping", name, *value,
                         min, max);
    *value = (*value < min) ? min : max;
  }
}

static void config_validate_dimensions(config_t *config) {
  config_clamp_int(&config->cat_height, MIN_CAT_HEIGHT, MAX_CAT_HEIGHT,
                   "cat_height");
  config_clamp_int(&config->overlay_height, MIN_OVERLAY_HEIGHT,
                   MAX_OVERLAY_HEIGHT, "overlay_height");
}

static void config_validate_timing(config_t *config) {
  config_clamp_int(&config->fps, MIN_FPS, MAX_FPS, "fps");
  config_clamp_int(&config->keypress_duration, MIN_DURATION, MAX_DURATION,
                   "keypress_duration");
  config_clamp_int(&config->test_animation_duration, MIN_DURATION, MAX_DURATION,
                   "test_animation_duration");

  // Validate interval (0 is allowed to disable)
  if (config->test_animation_interval < 0 ||
      config->test_animation_interval > MAX_INTERVAL) {
    validation_failed = true;
    bongocat_log_warning(
        "test_animation_interval %d out of range [0-%d], clamping",
        config->test_animation_interval, MAX_INTERVAL);
    config->test_animation_interval =
        (config->test_animation_interval < 0) ? 0 : MAX_INTERVAL;
  }

  if (config->hotplug_scan_interval < 0 ||
      config->hotplug_scan_interval > MAX_INTERVAL) {
    validation_failed = true;
    bongocat_log_warning(
        "hotplug_scan_interval %d out of range [0-%d], clamping",
        config->hotplug_scan_interval, MAX_INTERVAL);
    config->hotplug_scan_interval =
        (config->hotplug_scan_interval < 0) ? 0 : MAX_INTERVAL;
  }

  if (config->idle_sleep_timeout_sec < 0 ||
      config->idle_sleep_timeout_sec > MAX_INTERVAL) {
    validation_failed = true;
    bongocat_log_warning("idle_sleep_timeout %d out of range [0-%d], clamping",
                         config->idle_sleep_timeout_sec, MAX_INTERVAL);
    config->idle_sleep_timeout_sec =
        config->idle_sleep_timeout_sec < 0 ? 0 : MAX_INTERVAL;
  }
}

static void config_validate_appearance(config_t *config) {
  // Validate opacity
  config_clamp_int(&config->overlay_opacity, 0, 255, "overlay_opacity");

  // Validate idle frame
  if (config->idle_frame < 0 || config->idle_frame >= NUM_FRAMES) {
    validation_failed = true;
    bongocat_log_warning("idle_frame %d out of range [0-%d], resetting to 0",
                         config->idle_frame, NUM_FRAMES - 1);
    config->idle_frame = 0;
  }
}

static void config_validate_enums(config_t *config) {
  // Validate layer
  if (config->layer < LAYER_BACKGROUND || config->layer > LAYER_OVERLAY) {
    bongocat_log_warning("Invalid layer %d, resetting to top", config->layer);
    config->layer = LAYER_TOP;
  }

  // Validate overlay_position
  if (config->overlay_position != POSITION_TOP &&
      config->overlay_position != POSITION_BOTTOM) {
    bongocat_log_warning("Invalid overlay_position %d, resetting to top",
                         config->overlay_position);
    config->overlay_position = POSITION_TOP;
  }
}

static void config_validate_positioning(config_t *config) {
  // Validate cat positioning doesn't go off-screen
  if (llabs((long long)config->cat_x_offset) > config->screen_width) {
    bongocat_log_warning(
        "cat_x_offset %d may position cat off-screen (screen width: %d)",
        config->cat_x_offset, config->screen_width);
  }
}

static void config_validate_time(config_t *config) {
  if (config->enable_scheduled_sleep) {
    const int begin_minutes =
        (config->sleep_begin.hour * 60) + config->sleep_begin.min;
    const int end_minutes =
        (config->sleep_end.hour * 60) + config->sleep_end.min;

    if (begin_minutes == end_minutes) {
      bongocat_log_warning("Sleep mode is enabled, but time is equal: "
                           "%02d:%02d, disable sleep mode",
                           config->sleep_begin.hour, config->sleep_begin.min);

      config->enable_scheduled_sleep = 0;
    }
  }
}

static bongocat_error_t config_validate(config_t *config) {
  BONGOCAT_CHECK_NULL(config, BONGOCAT_ERROR_INVALID_PARAM);

  // Normalize boolean values
  config->enable_debug = config->enable_debug ? 1 : 0;
  config->enable_scheduled_sleep = config->enable_scheduled_sleep ? 1 : 0;

  config_validate_dimensions(config);
  config_validate_timing(config);
  config_validate_appearance(config);
  config_validate_enums(config);
  config_validate_positioning(config);

  // Normalize boolean values
  config->mirror_x = config->mirror_x ? 1 : 0;
  config->mirror_y = config->mirror_y ? 1 : 0;
  config->enable_antialiasing = config->enable_antialiasing ? 1 : 0;
  config_validate_time(config);
  return BONGOCAT_SUCCESS;
}

// =============================================================================
// DEVICE MANAGEMENT MODULE
// =============================================================================

static bongocat_error_t config_expand_array(char ***array_ptr, int *count,
                                            const char *str) {
  char **new_array =
      (char **)realloc((void *)*array_ptr, (*count + 1) * sizeof(char *));
  if (!new_array) {
    return BONGOCAT_ERROR_MEMORY;
  }
  *array_ptr = new_array;

  size_t len = strlen(str);
  (*array_ptr)[*count] = malloc(len + 1);
  if (!(*array_ptr)[*count]) {
    return BONGOCAT_ERROR_MEMORY;
  }

  memcpy((*array_ptr)[*count], str, len + 1);  // includes null terminator
  (*count)++;

  return BONGOCAT_SUCCESS;
}

static bongocat_error_t config_add_keyboard_device(config_t *config,
                                                   const char *device_path) {
  bongocat_error_t err = config_expand_array(
      &config->keyboard_devices, &config->num_keyboard_devices, device_path);
  if (err != BONGOCAT_SUCCESS) {
    bongocat_log_error("Failed to add keyboard device: %s",
                       bongocat_error_string(err));
    return err;
  }

  return BONGOCAT_SUCCESS;
}

static void config_free_string_array(char ***array_ptr, int *count) {
  if (*array_ptr) {
    for (int i = 0; i < *count; i++) {
      free((*array_ptr)[i]);
    }
    free((void *)*array_ptr);
    *array_ptr = NULL;
    *count = 0;
  }
}

static void config_cleanup_devices(config_t *config) {
  if (!config) {
    return;
  }

  config_free_string_array(&config->keyboard_devices,
                           &config->num_keyboard_devices);
  config_free_string_array(&config->keyboard_names, &config->num_names);
}

// =============================================================================
// CONFIGURATION PARSING MODULE
// =============================================================================

static char *config_trim_whitespace(char *text) {
  while (*text == ' ' || *text == '\t') {
    text++;
  }

  if (*text == '\0') {
    return text;
  }

  char *end = text + strlen(text) - 1;
  while (end > text && (*end == ' ' || *end == '\t')) {
    *end = '\0';
    end--;
  }

  return text;
}

static bool config_parse_int(const char *str, int *out) {
  if (!str || str[0] == '\0') {
    return false;
  }
  errno = 0;
  char *endptr;
  long val = strtol(str, &endptr, 10);
  if (errno != 0 || endptr == str) {
    return false;
  }
  while (*endptr == ' ' || *endptr == '\t') {
    endptr++;
  }
  if (*endptr != '\0') {
    return false;
  }
  if (val < INT_MIN || val > INT_MAX) {
    return false;
  }
  *out = (int)val;
  return true;
}

static bongocat_error_t
config_parse_integer_key(config_t *config, const char *key, const char *value) {
  // Identify which field this key maps to (NULL = not an integer key)
  int *target = NULL;
  if (strcmp(key, "cat_x_offset") == 0) {
    target = &config->cat_x_offset;
  } else if (strcmp(key, "cat_y_offset") == 0) {
    target = &config->cat_y_offset;
  } else if (strcmp(key, "cat_height") == 0) {
    target = &config->cat_height;
  } else if (strcmp(key, "overlay_height") == 0) {
    target = &config->overlay_height;
  } else if (strcmp(key, "idle_frame") == 0) {
    target = &config->idle_frame;
  } else if (strcmp(key, "keypress_duration") == 0) {
    target = &config->keypress_duration;
  } else if (strcmp(key, "test_animation_duration") == 0) {
    target = &config->test_animation_duration;
  } else if (strcmp(key, "test_animation_interval") == 0) {
    target = &config->test_animation_interval;
  } else if (strcmp(key, "fps") == 0) {
    target = &config->fps;
  } else if (strcmp(key, "overlay_opacity") == 0) {
    target = &config->overlay_opacity;
  } else if (strcmp(key, "mirror_x") == 0) {
    target = &config->mirror_x;
  } else if (strcmp(key, "mirror_y") == 0) {
    target = &config->mirror_y;
  } else if (strcmp(key, "enable_antialiasing") == 0) {
    target = &config->enable_antialiasing;
  } else if (strcmp(key, "enable_hand_mapping") == 0) {
    target = &config->enable_hand_mapping;
  } else if (strcmp(key, "enable_debug") == 0) {
    target = &config->enable_debug;
  } else if (strcmp(key, "enable_scheduled_sleep") == 0) {
    target = &config->enable_scheduled_sleep;
  } else if (strcmp(key, "idle_sleep_timeout") == 0) {
    target = &config->idle_sleep_timeout_sec;
  } else if (strcmp(key, "hotplug_scan_interval") == 0) {
    target = &config->hotplug_scan_interval;
  } else if (strcmp(key, "disable_fullscreen_hide") == 0) {
    target = &config->disable_fullscreen_hide;
  }

  if (!target) {
    return BONGOCAT_ERROR_INVALID_PARAM;  // Not an integer key
  }

  int int_value;
  if (!config_parse_int(value, &int_value)) {
    bongocat_log_warning("Invalid integer value '%s' for key '%s'", value, key);
    return BONGOCAT_ERROR_INVALID_PARAM;
  }

  bool boolean_key =
      (target == &config->mirror_x || target == &config->mirror_y ||
       target == &config->enable_antialiasing ||
       target == &config->enable_hand_mapping ||
       target == &config->enable_debug ||
       target == &config->enable_scheduled_sleep ||
       target == &config->disable_fullscreen_hide) != 0;
  if (boolean_key && int_value != 0 && int_value != 1) {
    bongocat_log_warning("Invalid boolean value '%s' for key '%s'", value, key);
    return BONGOCAT_ERROR_INVALID_PARAM;
  }

  struct {
    const char *key;
    int minimum, maximum;
  } ranges[] = {
      {"cat_height",              10, 200           },
      {"overlay_height",          20, 300           },
      {"fps",                     1,  120           },
      {"overlay_opacity",         0,  255           },
      {"idle_frame",              0,  NUM_FRAMES - 1},
      {"keypress_duration",       10, 5000          },
      {"test_animation_duration", 10, 5000          },
      {"test_animation_interval", 0,  3600          },
      {"hotplug_scan_interval",   0,  3600          },
      {"idle_sleep_timeout",      0,  3600          }
  };
  for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
    if (strcmp(key, ranges[i].key) == 0 &&
        (int_value < ranges[i].minimum || int_value > ranges[i].maximum)) {
      diagnostic(key, "value outside allowed range");
      if (strict_parse) {
        return BONGOCAT_ERROR_CONFIG;
      }
    }
  }
  *target = int_value;
  return BONGOCAT_SUCCESS;
}

static bongocat_error_t config_parse_enum_key(config_t *config, const char *key,
                                              const char *value) {
  if (strcmp(key, "layer") == 0) {
    if (strcmp(value, "background") == 0) {
      config->layer = LAYER_BACKGROUND;
    } else if (strcmp(value, "bottom") == 0) {
      config->layer = LAYER_BOTTOM;
    } else if (strcmp(value, "top") == 0) {
      config->layer = LAYER_TOP;
    } else if (strcmp(value, "overlay") == 0) {
      config->layer = LAYER_OVERLAY;
    } else {
      if (strict_parse) {
        return BONGOCAT_ERROR_CONFIG;
      }
      bongocat_log_warning("Invalid layer '%s', using 'top'", value);
      config->layer = LAYER_TOP;
    }
  } else if (strcmp(key, "overlay_position") == 0) {
    if (strcmp(value, "top") == 0) {
      config->overlay_position = POSITION_TOP;
    } else if (strcmp(value, "bottom") == 0) {
      config->overlay_position = POSITION_BOTTOM;
    } else {
      if (strict_parse) {
        return BONGOCAT_ERROR_CONFIG;
      }
      bongocat_log_warning("Invalid overlay_position '%s', using 'top'", value);
      config->overlay_position = POSITION_TOP;
    }
  } else if (strcmp(key, "cat_align") == 0) {
    if (strcmp(value, "left") == 0) {
      config->cat_align = ALIGN_LEFT;
    } else if (strcmp(value, "center") == 0) {
      config->cat_align = ALIGN_CENTER;
    } else if (strcmp(value, "right") == 0) {
      config->cat_align = ALIGN_RIGHT;
    } else {
      if (strict_parse) {
        return BONGOCAT_ERROR_CONFIG;
      }
      bongocat_log_warning("Invalid cat_align '%s', using 'center'", value);
      config->cat_align = ALIGN_CENTER;
    }
  } else {
    return BONGOCAT_ERROR_INVALID_PARAM;  // Unknown key
  }

  return BONGOCAT_SUCCESS;
}

static bongocat_error_t config_parse_time_key(config_t *config, const char *key,
                                              const char *value) {
  // Only try to parse time for time-related keys
  if (strcmp(key, "sleep_begin") != 0 && strcmp(key, "sleep_end") != 0) {
    return BONGOCAT_ERROR_INVALID_PARAM;  // Not a time key
  }

  if (strlen(value) != 5 || (value[0] < '0' || value[0] > '9') ||
      (value[1] < '0' || value[1] > '9') || value[2] != ':' ||
      (value[3] < '0' || value[3] > '9') ||
      (value[4] < '0' || value[4] > '9')) {
    bongocat_log_warning("Invalid time format '%s', expected HH:MM", value);
    return BONGOCAT_ERROR_INVALID_PARAM;
  }
  int hour = ((value[0] - '0') * 10) + (value[1] - '0');
  int min = ((value[3] - '0') * 10) + (value[4] - '0');

  if (hour < 0 || hour > 23 || min < 0 || min > 59) {
    bongocat_log_warning(
        "Invalid time values '%s', hour must be 0-23, minute must be 0-59",
        value);
    return BONGOCAT_ERROR_INVALID_PARAM;
  }

  if (strcmp(key, "sleep_begin") == 0) {
    config->sleep_begin.hour = hour;
    config->sleep_begin.min = min;
  } else if (strcmp(key, "sleep_end") == 0) {
    config->sleep_end.hour = hour;
    config->sleep_end.min = min;
  }

  return BONGOCAT_SUCCESS;
}

static bongocat_error_t config_parse_monitor_list(config_t *config,
                                                  const char *value) {
  config_free_string_array(&config->output_names, &config->num_output_names);
  free(config->output_name);
  config->output_name = NULL;

  char *monitor_list = strdup(value);
  if (!monitor_list) {
    return BONGOCAT_ERROR_MEMORY;
  }

  char *saveptr = NULL;
  char *token = strtok_r(monitor_list, ",", &saveptr);
  while (token) {
    char *monitor_name = config_trim_whitespace(token);
    if (monitor_name[0] != '\0') {
      bongocat_error_t err = config_expand_array(
          &config->output_names, &config->num_output_names, monitor_name);
      if (err != BONGOCAT_SUCCESS) {
        free(monitor_list);
        return err;
      }
    }

    token = strtok_r(NULL, ",", &saveptr);
  }

  free(monitor_list);

  if (config->num_output_names > 0) {
    config->output_name = strdup(config->output_names[0]);
    if (!config->output_name) {
      return BONGOCAT_ERROR_MEMORY;
    }
  } else {
    bongocat_log_warning(
        "monitor is empty, falling back to automatic output selection");
  }

  return BONGOCAT_SUCCESS;
}

static bongocat_error_t
config_parse_string_key(config_t *config, const char *key, const char *value) {
  if (strcmp(key, "monitor") == 0) {
    return config_parse_monitor_list(config, value);
  } else if (strcmp(key, "keyboard_name") == 0) {
    return config_expand_array(&config->keyboard_names, &config->num_names,
                               value);
  } else {
    return BONGOCAT_ERROR_INVALID_PARAM;  // Unknown key
  }
}

static bongocat_error_t
config_parse_key_value(config_t *config, const char *key, const char *value) {
  // Try integer keys first
  bongocat_error_t integer_result =
      config_parse_integer_key(config, key, value);
  if (integer_result != BONGOCAT_ERROR_INVALID_PARAM) {
    return integer_result;
  }

  // Try enum keys
  bongocat_error_t enum_result = config_parse_enum_key(config, key, value);
  if (enum_result != BONGOCAT_ERROR_INVALID_PARAM) {
    return enum_result;
  }

  // Try time keys
  if (config_parse_time_key(config, key, value) == BONGOCAT_SUCCESS) {
    return BONGOCAT_SUCCESS;
  }

  // Try string keys
  bongocat_error_t string_result = config_parse_string_key(config, key, value);
  if (string_result != BONGOCAT_ERROR_INVALID_PARAM) {
    return string_result;
  }

  // Handle device keys
  if (strcmp(key, "keyboard_device") == 0 ||
      strcmp(key, "keyboard_devices") == 0) {
    // Validate path starts with /dev/input/ and has no traversal
    if (strncmp(value, "/dev/input/", 11) != 0) {
      bongocat_log_warning(
          "keyboard_device path must start with /dev/input/: %s", value);
      return BONGOCAT_ERROR_INVALID_PARAM;
    }
    if (strstr(value, "..") != NULL) {
      bongocat_log_warning("Path traversal detected in device path: %s", value);
      return BONGOCAT_ERROR_INVALID_PARAM;
    }
    return config_add_keyboard_device(config, value);
  }

  // Unknown key
  return BONGOCAT_ERROR_INVALID_PARAM;
}

static bool config_is_comment_or_empty(const char *line) {
  const unsigned char *p = (const unsigned char *)line;
  while (*p == ' ' || *p == '\t') {
    p++;
  }
  return (*p == '#' || *p == ';' || *p == '\0') != 0;
}

static bool config_parse_line(char *line, char **out_key, char **out_value) {
  char *equals = strchr(line, '=');
  if (!equals) {
    return false;
  }

  *equals = '\0';
  *out_key = config_trim_whitespace(line);
  *out_value = config_trim_whitespace(equals + 1);

  // Support inline comments: key=value # comment
  if ((*out_value)[0] == '#') {
    (*out_value)[0] = '\0';
  } else {
    char *space_comment = strstr(*out_value, " #");
    char *tab_comment = strstr(*out_value, "\t#");
    char *comment_start = NULL;

    if (space_comment && tab_comment) {
      comment_start =
          (space_comment < tab_comment) ? space_comment : tab_comment;
    } else if (space_comment) {
      comment_start = space_comment;
    } else if (tab_comment) {
      comment_start = tab_comment;
    }

    if (comment_start) {
      comment_start[1] = '\0';
      *out_value = config_trim_whitespace(*out_value);
    }
  }

  return (*out_key)[0] != '\0';
}

static bongocat_error_t config_parse_monitor_setting(config_t *config,
                                                     const char *monitor,
                                                     const char *key,
                                                     const char *value) {
  const char *allowed[] = {
      "cat_height",       "overlay_height",      "overlay_opacity",
      "cat_x_offset",     "cat_y_offset",        "layer",
      "overlay_position", "cat_align",           "mirror_x",
      "mirror_y",         "enable_antialiasing", "disable_fullscreen_hide"};
  bool appearance = false;
  for (size_t k = 0; k < sizeof(allowed) / sizeof(allowed[0]); k++) {
    appearance |= strcmp(key, allowed[k]) == 0;
  }
  config_t probe = *config;
  bongocat_error_t parse_result =
      (int)appearance ? config_parse_key_value(&probe, key, value)
                      : BONGOCAT_ERROR_INVALID_PARAM;
  if (parse_result == BONGOCAT_SUCCESS) {
    bool previous = validation_failed;
    validation_failed = false;
    bongocat_error_t valid = config_validate(&probe);
    bool invalid = validation_failed;
    validation_failed |= previous;
    if (valid != BONGOCAT_SUCCESS || (strict_parse && invalid)) {
      parse_result = BONGOCAT_ERROR_CONFIG;
    }
  }
  if (parse_result == BONGOCAT_SUCCESS) {
    monitor_override_t entry = {
        .monitor = strdup(monitor), .key = strdup(key), .value = strdup(value)};
    monitor_override_t *entries = NULL;
    if (entry.monitor && entry.key && entry.value) {
      entries = realloc(config->overrides,
                        (config->num_overrides + 1) * sizeof(*entries));
    }
    if (!entries) {
      free(entry.monitor);
      free(entry.key);
      free(entry.value);
      parse_result = BONGOCAT_ERROR_MEMORY;
    } else {
      config->overrides = entries;
      config->overrides[config->num_overrides++] = entry;
    }
  }
  return parse_result;
}

static bool config_parse_section(char *trimmed, char *section,
                                 size_t section_size) {
  size_t n = strlen(trimmed);
  if (n > 2 && trimmed[n - 1] == ']') {
    trimmed[n - 1] = '\0';
    if (strcmp(trimmed + 1, "global") == 0) {
      section[0] = '\0';
      return true;
    }
    if (strncmp(trimmed + 1, "monitor:", 8) == 0 && strlen(trimmed + 9) > 0 &&
        strlen(trimmed + 9) < section_size) {
      snprintf(section, section_size, "%s", trimmed + 9);
      return true;
    }
  }
  return false;
}

static const char *config_startup_path(const char *config_file_path,
                                       char resolved[PATH_MAX]) {
  const char *file_path = config_file_path ? config_file_path : "bongocat.conf";

  // If no explicit path, try XDG paths
  bool using_resolved = false;

  if (!config_file_path) {
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");

    if (xdg_config && xdg_config[0] != '\0') {
      snprintf(resolved, PATH_MAX, "%s/bongocat/bongocat.conf", xdg_config);
      if (access(resolved, R_OK) == 0) {
        file_path = resolved;
        using_resolved = true;
      }
    }

    if (!using_resolved && home && home[0] != '\0') {
      snprintf(resolved, PATH_MAX, "%s/.config/bongocat/bongocat.conf", home);
      if (access(resolved, R_OK) == 0) {
        file_path = resolved;
      }
    }
  }

  return file_path;
}

static bongocat_error_t config_parse_file(config_t *config,
                                          const char *config_file_path) {
  BONGOCAT_CHECK_NULL(config, BONGOCAT_ERROR_INVALID_PARAM);

  char resolved[PATH_MAX];
  const char *file_path = config_startup_path(config_file_path, resolved);

  snprintf(diagnostic_path, sizeof(diagnostic_path), "%s", file_path);
  diagnostic_line = 0;
  FILE *file = fopen(file_path, "r");
  if (!file) {
    if (strict_parse) {
      diagnostic(NULL, "cannot open configuration");
      return BONGOCAT_ERROR_FILE_IO;
    }
    bongocat_log_info("Config file '%s' not found, using defaults", file_path);
    return BONGOCAT_SUCCESS;
  }

  char line[512];
  int line_number = 0;
  char section[128] = "";
  bongocat_error_t result = BONGOCAT_SUCCESS;

  while (fgets(line, sizeof(line), file)) {
    line_number++;
    diagnostic_line = line_number;

    // Reject truncated lines rather than interpreting fragments as settings.
    size_t len = strlen(line);
    if (len == sizeof(line) - 1 && line[len - 1] != '\n') {
      int next;
      while ((next = fgetc(file)) != '\n' && next != EOF) {}
      diagnostic(NULL, "line too long");
      if (strict_parse) {
        result = BONGOCAT_ERROR_CONFIG;
      }
      if (next == EOF) {
        break;
      }
      continue;
    }
    // Remove trailing newline
    if (len > 0 && line[len - 1] == '\n') {
      line[len - 1] = '\0';
    }

    // Skip comments and empty lines
    if (config_is_comment_or_empty(line)) {
      continue;
    }

    char *trimmed = config_trim_whitespace(line);
    if (trimmed[0] == '[') {
      if (config_parse_section(trimmed, section, sizeof(section))) {
        continue;
      }
      diagnostic(NULL, "invalid section");
      if (strict_parse) {
        result = BONGOCAT_ERROR_CONFIG;
      }
      continue;
    }
    // Parse key=value pairs
    char *key = NULL;
    char *value = NULL;
    if (config_parse_line(line, &key, &value)) {
      bongocat_error_t parse_result;
      if (section[0]) {
        parse_result =
            config_parse_monitor_setting(config, section, key, value);
      } else {
        parse_result = config_parse_key_value(config, key, value);
      }
      if (parse_result == BONGOCAT_ERROR_INVALID_PARAM) {
        diagnostic(key, "unknown key or invalid value");
        if (strict_parse) {
          result = BONGOCAT_ERROR_CONFIG;
        }
      } else if (parse_result != BONGOCAT_SUCCESS) {
        diagnostic(key, parse_result == BONGOCAT_ERROR_MEMORY
                            ? "allocation failed"
                            : "invalid value");
        result = parse_result;
        break;
      }
    } else if (strlen(line) > 0) {
      diagnostic(NULL, "malformed configuration line");
      if (strict_parse) {
        result = BONGOCAT_ERROR_CONFIG;
      }
    }
  }

  if (ferror(file)) {
    result = BONGOCAT_ERROR_FILE_IO;
  }
  fclose(file);

  if (result == BONGOCAT_SUCCESS) {
    bongocat_log_info("Loaded configuration from %s", file_path);
  }

  return result;
}

// =============================================================================
// DEFAULT CONFIGURATION MODULE
// =============================================================================

static void config_set_defaults(config_t *config) {
  *config = (config_t){
      .screen_width =
          DEFAULT_SCREEN_WIDTH, // Will be updated by Wayland detection
      .output_name = NULL, // Will default to automatic one if kept null
      .output_names = NULL,
      .num_output_names = 0,
      .asset_paths = {"assets/new/bongo-both-up.svg",
                      "assets/new/bongo-left-down.svg", "assets/new/bongo-right-down.svg",
                      "assets/new/bongo-both-down.svg", "assets/new/bongo-sleeping.svg"},
      .keyboard_devices = NULL,
      .num_keyboard_devices = 0,
      .hotplug_scan_interval = 30,
      .keyboard_names = NULL,
      .num_names = 0,
      .cat_x_offset = 100,
      .cat_y_offset = 10,
      .cat_height = 40,
      .overlay_height = 50,
      .idle_frame = 0,
      .keypress_duration = 100,
      .test_animation_duration = 200,
      .test_animation_interval = 0,
      .fps = 60,
      .overlay_opacity = 150,
      .mirror_x = 0,
      .mirror_y = 0,
      .enable_antialiasing = 1,
      .enable_hand_mapping = 1, // Enabled by default
      .enable_debug = 0,
      .layer = LAYER_TOP, // Default to TOP for broader compatibility
      .overlay_position = POSITION_TOP,
      .cat_align = ALIGN_CENTER,
      .enable_scheduled_sleep = 0,
      .sleep_begin = (config_time_t){0, 0},
      .sleep_end = (config_time_t){0, 0},
      .idle_sleep_timeout_sec = 0,
      .disable_fullscreen_hide = 0,
  };
}

static void config_finalize(config_t *config) {
  // Initialize error system with debug setting
  bongocat_error_init(config->enable_debug);
}

static void config_log_summary(const config_t *config) {
  bongocat_log_debug("Configuration loaded successfully");
  bongocat_log_debug("  Screen: %dx%d", config->screen_width,
                     config->overlay_height);
  bongocat_log_debug("  Cat: %dx%d at offset (%d,%d)", config->cat_height,
                     (config->cat_height * CAT_IMAGE_WIDTH) / CAT_IMAGE_HEIGHT,
                     config->cat_x_offset, config->cat_y_offset);
  bongocat_log_debug("  FPS: %d, Opacity: %d", config->fps,
                     config->overlay_opacity);
  bongocat_log_debug("  Mirror: X=%d, Y=%d", config->mirror_x,
                     config->mirror_y);
  bongocat_log_debug("  Anti-aliasing: %s",
                     config->enable_antialiasing ? "enabled" : "disabled");
  bongocat_log_debug("  Position: %s", config->overlay_position == POSITION_TOP
                                           ? "top"
                                           : "bottom");
  bongocat_log_debug("  Layer: %s",
                     config->layer == LAYER_TOP ? "top" : "overlay");
  bongocat_log_debug("  Monitors: %d configured", config->num_output_names);
}

// =============================================================================
// PUBLIC API IMPLEMENTATION
// =============================================================================

bongocat_error_t load_config(config_t *config, const char *config_file_path) {
  BONGOCAT_CHECK_NULL(config, BONGOCAT_ERROR_INVALID_PARAM);

  validation_failed = false;
  // Initialize with defaults
  config_set_defaults(config);

  // Parse config file and override defaults
  bongocat_error_t result = config_parse_file(config, config_file_path);
  if (result != BONGOCAT_SUCCESS) {
    bongocat_log_error("Failed to parse configuration file: %s",
                       bongocat_error_string(result));
    return result;
  }

  // Validate and sanitize configuration
  result = config_validate(config);
  if (result != BONGOCAT_SUCCESS) {
    bongocat_log_error("Configuration validation failed: %s",
                       bongocat_error_string(result));
    return result;
  }

  if (strict_parse && validation_failed) {
    return BONGOCAT_ERROR_CONFIG;
  }

  // Finalize configuration
  config_finalize(config);

  // Log configuration summary
  config_log_summary(config);

  return BONGOCAT_SUCCESS;
}

void config_cleanup(void) {
  // No global config state to clean up.
}

void config_cleanup_full(config_t *config) {
  if (!config) {
    return;
  }

  for (size_t i = 0; i < config->num_overrides; i++) {
    free(config->overrides[i].monitor);
    free(config->overrides[i].key);
    free(config->overrides[i].value);
  }
  free(config->overrides);
  config->overrides = NULL;
  config->num_overrides = 0;
  config_cleanup_devices(config);

  if (config->output_name) {
    free(config->output_name);
    config->output_name = NULL;
  }

  config_free_string_array(&config->output_names, &config->num_output_names);
}

int get_screen_width(void) {
  // This function is now only used for initial config loading
  // The actual screen width detection happens in wayland_init
  return DEFAULT_SCREEN_WIDTH;
}

char *config_resolve_path(const char *explicit_path) {
  if (explicit_path) {
    return strdup(explicit_path);
  }

  char path[PATH_MAX];

  // 1. $XDG_CONFIG_HOME/bongocat/bongocat.conf
  const char *xdg_config = getenv("XDG_CONFIG_HOME");
  if (xdg_config && xdg_config[0] != '\0') {
    snprintf(path, sizeof(path), "%s/bongocat/bongocat.conf", xdg_config);
    if (access(path, R_OK) == 0) {
      return strdup(path);
    }
  }

  // 2. ~/.config/bongocat/bongocat.conf
  const char *home = getenv("HOME");
  if (home && home[0] != '\0') {
    snprintf(path, sizeof(path), "%s/.config/bongocat/bongocat.conf", home);
    if (access(path, R_OK) == 0) {
      return strdup(path);
    }
  }

  // 3. ./bongocat.conf (CWD)
  if (access("bongocat.conf", R_OK) == 0) {
    return strdup("bongocat.conf");
  }

  // No config found — will use defaults
  return NULL;
}

bongocat_error_t load_config_strict(config_t *config, const char *path) {
  strict_parse = true;
  bongocat_error_t result = load_config(config, path);
  strict_parse = false;
  return result;
}

void config_for_monitor(const config_t *global, const char *name,
                        config_t *effective) {
  *effective = *global;
  for (size_t i = 0; i < global->num_overrides; i++) {
    const monitor_override_t *entry = &global->overrides[i];
    if (name && strcmp(entry->monitor, name) == 0) {
      bongocat_error_t result =
          config_parse_key_value(effective, entry->key, entry->value);
      (void)result;
    }
  }
  bongocat_error_t result = config_validate(effective);
  (void)result;
}

bongocat_error_t load_config_report(config_t *config, const char *path,
                                    bool strict,
                                    config_diagnostic_callback_t callback,
                                    void *data) {
  diagnostic_callback = callback;
  diagnostic_data = data;
  bongocat_error_t result = (int)strict ? load_config_strict(config, path)
                                        : load_config(config, path);
  diagnostic_callback = NULL;
  diagnostic_data = NULL;
  return result;
}
