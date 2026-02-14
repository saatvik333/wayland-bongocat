#define _POSIX_C_SOURCE 200809L
#include "config/config.h"

#include "utils/error.h"
#include "utils/memory.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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

static void config_clamp_int(int *value, int min, int max, const char *name) {
  if (*value < min || *value > max) {
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
    bongocat_log_warning(
        "test_animation_interval %d out of range [0-%d], clamping",
        config->test_animation_interval, MAX_INTERVAL);
    config->test_animation_interval =
        (config->test_animation_interval < 0) ? 0 : MAX_INTERVAL;
  }

  if (config->hotplug_scan_interval < 0 ||
      config->hotplug_scan_interval > MAX_INTERVAL) {
    bongocat_log_warning(
        "hotplug_scan_interval %d out of range [0-%d], clamping",
        config->hotplug_scan_interval, MAX_INTERVAL);
    config->hotplug_scan_interval =
        (config->hotplug_scan_interval < 0) ? 0 : MAX_INTERVAL;
  }
}

static void config_validate_appearance(config_t *config) {
  // Validate opacity
  config_clamp_int(&config->overlay_opacity, 0, 255, "overlay_opacity");

  // Validate idle frame
  if (config->idle_frame < 0 || config->idle_frame >= NUM_FRAMES) {
    bongocat_log_warning("idle_frame %d out of range [0-%d], resetting to 0",
                         config->idle_frame, NUM_FRAMES - 1);
    config->idle_frame = 0;
  }
}

static void config_validate_enums(config_t *config) {
  // Validate layer
  if (config->layer != LAYER_TOP && config->layer != LAYER_OVERLAY) {
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
  if (abs(config->cat_x_offset) > config->screen_width) {
    bongocat_log_warning(
        "cat_x_offset %d may position cat off-screen (screen width: %d)",
        config->cat_x_offset, config->screen_width);
  }
}

static void config_validate_time(config_t *config) {
  if (config->enable_scheduled_sleep) {
    const int begin_minutes =
        config->sleep_begin.hour * 60 + config->sleep_begin.min;
    const int end_minutes = config->sleep_end.hour * 60 + config->sleep_end.min;

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
      bongocat_realloc(*array_ptr, (*count + 1) * sizeof(char *));
  if (!new_array) {
    return BONGOCAT_ERROR_MEMORY;
  }
  *array_ptr = new_array;

  size_t len = strlen(str);
  (*array_ptr)[*count] = BONGOCAT_MALLOC(len + 1);
  if (!(*array_ptr)[*count]) {
    return BONGOCAT_ERROR_MEMORY;
  }

  strncpy((*array_ptr)[*count], str, len);
  (*array_ptr)[*count][len] = '\0';
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

static bongocat_error_t config_resolve_devices(config_t *config) {
  if (config->num_names == 0) {
    return BONGOCAT_SUCCESS;
  }

  DIR *dir = opendir("/dev/input");
  if (!dir) {
    bongocat_log_warning("Failed to open /dev/input for scanning: %s",
                         strerror(errno));
    return BONGOCAT_ERROR_FILE_IO;
  }

  struct dirent *entry;
  char path[PATH_MAX];
  char name[256] = {0};

  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, "event", 5) != 0) {
      continue;
    }

    snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
      continue;
    }

    bool matched = false;
    memset(name, 0, sizeof(name));
    if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0) {
      name[sizeof(name) - 1] = '\0';
      for (int i = 0; i < config->num_names; i++) {
        if (strstr(name, config->keyboard_names[i]) != NULL) {
          bongocat_log_info(
              "Found device matching name '%s' (Device: '%s'): %s",
              config->keyboard_names[i], name, path);
          matched = true;
          break;
        }
      }
    }

    if (matched) {
      config_add_keyboard_device(config, path);
    }

    close(fd);
  }

  closedir(dir);
  return BONGOCAT_SUCCESS;
}

static void config_free_string_array(char ***array_ptr, int *count) {
  if (*array_ptr) {
    for (int i = 0; i < *count; i++) {
      BONGOCAT_SAFE_FREE((*array_ptr)[i]);
    }
    BONGOCAT_SAFE_FREE(*array_ptr);
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

static bongocat_error_t
config_parse_integer_key(config_t *config, const char *key, const char *value) {
  int int_value = (int)strtol(value, NULL, 10);

  if (strcmp(key, "cat_x_offset") == 0) {
    config->cat_x_offset = int_value;
  } else if (strcmp(key, "cat_y_offset") == 0) {
    config->cat_y_offset = int_value;
  } else if (strcmp(key, "cat_height") == 0) {
    config->cat_height = int_value;
  } else if (strcmp(key, "overlay_height") == 0) {
    config->overlay_height = int_value;
  } else if (strcmp(key, "idle_frame") == 0) {
    config->idle_frame = int_value;
  } else if (strcmp(key, "keypress_duration") == 0) {
    config->keypress_duration = int_value;
  } else if (strcmp(key, "test_animation_duration") == 0) {
    config->test_animation_duration = int_value;
  } else if (strcmp(key, "test_animation_interval") == 0) {
    config->test_animation_interval = int_value;
  } else if (strcmp(key, "fps") == 0) {
    config->fps = int_value;
  } else if (strcmp(key, "overlay_opacity") == 0) {
    config->overlay_opacity = int_value;
  } else if (strcmp(key, "mirror_x") == 0) {
    config->mirror_x = int_value;
  } else if (strcmp(key, "mirror_y") == 0) {
    config->mirror_y = int_value;
  } else if (strcmp(key, "enable_antialiasing") == 0) {
    config->enable_antialiasing = int_value;
  } else if (strcmp(key, "enable_hand_mapping") == 0) {
    config->enable_hand_mapping = int_value;
  } else if (strcmp(key, "enable_debug") == 0) {
    config->enable_debug = int_value;
  } else if (strcmp(key, "enable_scheduled_sleep") == 0) {
    config->enable_scheduled_sleep = int_value;
  } else if (strcmp(key, "idle_sleep_timeout") == 0) {
    config->idle_sleep_timeout_sec = int_value;
  } else if (strcmp(key, "hotplug_scan_interval") == 0) {
    config->hotplug_scan_interval = int_value;
  } else if (strcmp(key, "disable_fullscreen_hide") == 0) {
    config->disable_fullscreen_hide = int_value;
  } else {
    return BONGOCAT_ERROR_INVALID_PARAM;  // Unknown key
  }

  return BONGOCAT_SUCCESS;
}

static bongocat_error_t config_parse_enum_key(config_t *config, const char *key,
                                              const char *value) {
  if (strcmp(key, "layer") == 0) {
    if (strcmp(value, "top") == 0) {
      config->layer = LAYER_TOP;
    } else if (strcmp(value, "overlay") == 0) {
      config->layer = LAYER_OVERLAY;
    } else {
      bongocat_log_warning("Invalid layer '%s', using 'top'", value);
      config->layer = LAYER_TOP;
    }
  } else if (strcmp(key, "overlay_position") == 0) {
    if (strcmp(value, "top") == 0) {
      config->overlay_position = POSITION_TOP;
    } else if (strcmp(value, "bottom") == 0) {
      config->overlay_position = POSITION_BOTTOM;
    } else {
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

  int hour, min;
  if (sscanf(value, "%d:%d", &hour, &min) != 2) {
    bongocat_log_warning("Invalid time format '%s', expected HH:MM", value);
    return BONGOCAT_ERROR_INVALID_PARAM;
  }

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
  BONGOCAT_SAFE_FREE(config->output_name);

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

  return BONGOCAT_SUCCESS;
}

static bongocat_error_t
config_parse_key_value(config_t *config, const char *key, const char *value) {
  // Try integer keys first
  if (config_parse_integer_key(config, key, value) == BONGOCAT_SUCCESS) {
    return BONGOCAT_SUCCESS;
  }

  // Try enum keys
  if (config_parse_enum_key(config, key, value) == BONGOCAT_SUCCESS) {
    return BONGOCAT_SUCCESS;
  }

  // Try time keys
  if (config_parse_time_key(config, key, value) == BONGOCAT_SUCCESS) {
    return BONGOCAT_SUCCESS;
  }

  // Try string keys
  if (config_parse_string_key(config, key, value) == BONGOCAT_SUCCESS) {
    return BONGOCAT_SUCCESS;
  }

  // Handle device keys
  if (strcmp(key, "keyboard_device") == 0 ||
      strcmp(key, "keyboard_devices") == 0) {
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
  return (*p == '#' || *p == '\0');
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

static bongocat_error_t config_parse_file(config_t *config,
                                          const char *config_file_path) {
  BONGOCAT_CHECK_NULL(config, BONGOCAT_ERROR_INVALID_PARAM);

  const char *file_path = config_file_path ? config_file_path : "bongocat.conf";

  // If no explicit path, try XDG paths
  char resolved[PATH_MAX];
  bool using_resolved = false;

  if (!config_file_path) {
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");

    if (xdg_config && xdg_config[0] != '\0') {
      snprintf(resolved, sizeof(resolved), "%s/bongocat/bongocat.conf",
               xdg_config);
      if (access(resolved, R_OK) == 0) {
        file_path = resolved;
        using_resolved = true;
      }
    }

    if (!using_resolved && home && home[0] != '\0') {
      snprintf(resolved, sizeof(resolved), "%s/.config/bongocat/bongocat.conf",
               home);
      if (access(resolved, R_OK) == 0) {
        file_path = resolved;
      }
    }
  }

  FILE *file = fopen(file_path, "r");
  if (!file) {
    bongocat_log_info("Config file '%s' not found, using defaults", file_path);
    return BONGOCAT_SUCCESS;
  }

  char line[512];
  int line_number = 0;
  bongocat_error_t result = BONGOCAT_SUCCESS;

  while (fgets(line, sizeof(line), file)) {
    line_number++;

    // Remove trailing newline
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') {
      line[len - 1] = '\0';
    }

    // Skip comments and empty lines
    if (config_is_comment_or_empty(line)) {
      continue;
    }

    // Parse key=value pairs
    char *key = NULL;
    char *value = NULL;
    if (config_parse_line(line, &key, &value)) {
      bongocat_error_t parse_result =
          config_parse_key_value(config, key, value);
      if (parse_result == BONGOCAT_ERROR_INVALID_PARAM) {
        bongocat_log_warning("Unknown configuration key '%s' at line %d", key,
                             line_number);
      } else if (parse_result != BONGOCAT_SUCCESS) {
        result = parse_result;
        break;
      }
    } else if (strlen(line) > 0) {
      bongocat_log_warning("Invalid configuration line %d: %s", line_number,
                           line);
    }
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
      .bar_height = DEFAULT_BAR_HEIGHT,
      .asset_paths = {"assets/bongo-cat-both-up.png",
                      "assets/bongo-cat-left-down.png", "assets/bongo-cat-right-down.png",
                      "assets/bongo-cat-both-down.png"},
      .keyboard_devices = NULL,
      .num_keyboard_devices = 0,
      .hotplug_scan_interval = 300,
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

static bongocat_error_t config_set_default_devices(config_t *config) {
  if (config->num_keyboard_devices == 0) {
    const char *default_device = "/dev/input/event4";
    return config_add_keyboard_device(config, default_device);
  }
  return BONGOCAT_SUCCESS;
}

static void config_finalize(config_t *config) {
  // Update bar_height from config
  config->bar_height = config->overlay_height;

  // Initialize error system with debug setting
  bongocat_error_init(config->enable_debug);
}

static void config_log_summary(const config_t *config) {
  bongocat_log_debug("Configuration loaded successfully");
  bongocat_log_debug("  Screen: %dx%d", config->screen_width,
                     config->bar_height);
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

  // Initialize with defaults
  config_set_defaults(config);

  // Parse config file and override defaults
  bongocat_error_t result = config_parse_file(config, config_file_path);
  if (result != BONGOCAT_SUCCESS) {
    bongocat_log_error("Failed to parse configuration file: %s",
                       bongocat_error_string(result));
    return result;
  }

  // Resolve keyboard_name entries to device paths.
  // Continue on scan failure so static keyboard_device entries still work.
  result = config_resolve_devices(config);
  if (result != BONGOCAT_SUCCESS) {
    bongocat_log_warning("Failed to resolve keyboard names, continuing: %s",
                         bongocat_error_string(result));
  }

  // Set default keyboard device if none specified
  if (config->keyboard_devices == NULL || config->num_keyboard_devices == 0) {
    result = config_set_default_devices(config);
    if (result != BONGOCAT_SUCCESS) {
      bongocat_log_error("Failed to set default keyboard devices: %s",
                         bongocat_error_string(result));
      return result;
    }
  }

  // Validate and sanitize configuration
  result = config_validate(config);
  if (result != BONGOCAT_SUCCESS) {
    bongocat_log_error("Configuration validation failed: %s",
                       bongocat_error_string(result));
    return result;
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
