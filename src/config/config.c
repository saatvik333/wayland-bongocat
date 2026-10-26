#define _POSIX_C_SOURCE 200809L
#include "config/config.h"

#include "core/bongocat.h"
#include "utils/error.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// =============================================================================
// KEY TABLE
// =============================================================================

typedef enum {
  KEY_INT,
  KEY_BOOL,
  KEY_ENUM,
  KEY_TIME,
  KEY_MONITORS,
  KEY_DEVICE,
  KEY_NAME,
  KEY_OBSOLETE,
} key_kind_t;

typedef struct {
  const char *name;
  key_kind_t kind;
  size_t offset;  // Field in config_t
  int min, max;   // KEY_INT range; KEY_ENUM: value of choices[0] in min
  const char *const *choices;
} key_spec_t;

static const char *const LAYERS[] = {"background", "bottom", "top", "overlay",
                                     NULL};
static const char *const POSITIONS[] = {"top", "bottom", NULL};
static const char *const ALIGNMENTS[] = {"left", "center", "right", NULL};

#define LOOK(field)  (offsetof(config_t, look) + offsetof(appearance_t, field))
#define FIELD(field) offsetof(config_t, field)

// Appearance keys come first; per_monitor() relies on their offsets.
// clang-format off
static const key_spec_t KEYS[] = {
  {"cat_height",              KEY_INT,  LOOK(cat_height),       10,  200,     NULL},
  {"overlay_height",          KEY_INT,  LOOK(overlay_height),   20,  300,     NULL},
  {"overlay_opacity",         KEY_INT,  LOOK(overlay_opacity),  0,   255,     NULL},
  {"cat_x_offset",            KEY_INT,  LOOK(cat_x_offset), INT_MIN, INT_MAX, NULL},
  {"cat_y_offset",            KEY_INT,  LOOK(cat_y_offset), INT_MIN, INT_MAX, NULL},
  {"layer",                   KEY_ENUM, LOOK(layer),  LAYER_BACKGROUND, 0, LAYERS},
  {"overlay_position",        KEY_ENUM, LOOK(overlay_position), POSITION_TOP, 0,
                                                                        POSITIONS},
  {"cat_align",               KEY_ENUM, LOOK(cat_align),  ALIGN_LEFT, 0, ALIGNMENTS},
  {"mirror_x",                KEY_BOOL, LOOK(mirror_x),                0, 1, NULL},
  {"mirror_y",                KEY_BOOL, LOOK(mirror_y),                0, 1, NULL},
  {"disable_fullscreen_hide", KEY_BOOL, LOOK(disable_fullscreen_hide), 0, 1, NULL},
  {"monitor",                 KEY_MONITORS, 0,                         0, 0, NULL},
  {"follow_active_monitor",   KEY_BOOL, FIELD(follow_active_monitor),  0, 1, NULL},
  {"keyboard_device",         KEY_DEVICE, 0,                           0, 0, NULL},
  {"keyboard_name",           KEY_NAME, 0,                             0, 0, NULL},
  {"fps",                     KEY_INT,  FIELD(fps),                    1, 120, NULL},
  {"keypress_duration",       KEY_INT,  FIELD(keypress_duration),   10, 5000, NULL},
  {"idle_frame",              KEY_INT,  FIELD(idle_frame),  0, NUM_FRAMES - 1, NULL},
  {"test_animation_duration", KEY_INT, FIELD(test_animation_duration), 10, 5000,
                                                                             NULL},
  {"test_animation_interval", KEY_INT, FIELD(test_animation_interval), 0, 3600,
                                                                             NULL},
  {"enable_hand_mapping",     KEY_BOOL, FIELD(enable_hand_mapping),    0, 1, NULL},
  {"enable_scheduled_sleep",  KEY_BOOL, FIELD(enable_scheduled_sleep), 0, 1, NULL},
  {"sleep_begin",             KEY_TIME, FIELD(sleep_begin),            0, 0, NULL},
  {"sleep_end",               KEY_TIME, FIELD(sleep_end),              0, 0, NULL},
  {"idle_sleep_timeout",      KEY_INT,  FIELD(idle_sleep_timeout),  0, 3600, NULL},
  {"enable_debug",            KEY_BOOL, FIELD(enable_debug),           0, 1, NULL},
  {"enable_antialiasing",     KEY_OBSOLETE, 0,                         0, 0, NULL},
  {"hotplug_scan_interval",   KEY_OBSOLETE, 0,                         0, 0, NULL},
};
// clang-format on

#define APPEARANCE_FIELDS (sizeof(appearance_t) / sizeof(int))
static_assert(sizeof(appearance_t) == APPEARANCE_FIELDS * sizeof(int),
              "appearance_t must contain only int fields");
static_assert(APPEARANCE_FIELDS <= sizeof(unsigned) * CHAR_BIT,
              "override bitmask is too small");
static_assert(offsetof(config_t, look) == 0,
              "per-monitor keys are identified by offset");

static bool per_monitor(const key_spec_t *spec) {
  return (spec->kind == KEY_INT || spec->kind == KEY_BOOL ||
          spec->kind == KEY_ENUM) &&
         spec->offset < sizeof(appearance_t);
}

static const key_spec_t *find_key(const char *name) {
  for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
    if (strcmp(KEYS[i].name, name) == 0) {
      return &KEYS[i];
    }
  }
  return NULL;
}

// =============================================================================
// DEFAULTS
// =============================================================================

static const config_t DEFAULTS = {
    .look.cat_height = 40,
    .look.overlay_height = 50,
    .look.overlay_opacity = 150,
    .look.cat_x_offset = 100,
    .look.cat_y_offset = 10,
    .look.layer = LAYER_TOP,
    .look.overlay_position = POSITION_TOP,
    .look.cat_align = ALIGN_CENTER,
    .follow_active_monitor = 1,
    .fps = 60,
    .keypress_duration = 100,
    .test_animation_duration = 200,
    .enable_hand_mapping = 1,
};

// =============================================================================
// PARSING
// =============================================================================

typedef struct {
  const char *path;
  int line;
  bool strict;
  bool failed;
} parser_t;

static void report(parser_t *parser, bool error, const char *key,
                   const char *message) {
  error = error && parser->strict;
  parser->failed |= error;
  if (key) {
    bongocat_log_warning("%s:%d: %s: '%s': %s", parser->path, parser->line,
                         error ? "error" : "warning", key, message);
  } else {
    bongocat_log_warning("%s:%d: %s: %s", parser->path, parser->line,
                         error ? "error" : "warning", message);
  }
}

static char *trim(char *text) {
  while (isspace((unsigned char)*text)) {
    text++;
  }
  size_t length = strlen(text);
  while (length && isspace((unsigned char)text[length - 1])) {
    text[--length] = '\0';
  }
  return text;
}

static bool parse_int(const char *text, int *value) {
  errno = 0;
  char *end;
  long parsed = strtol(text, &end, 10);
  if (errno || end == text || *end || parsed < INT_MIN || parsed > INT_MAX) {
    return false;
  }
  *value = (int)parsed;
  return true;
}

static bool parse_time(const char *text, int *minutes) {
  if (strlen(text) != 5 || text[2] != ':' || !isdigit((unsigned char)text[0]) ||
      !isdigit((unsigned char)text[1]) || !isdigit((unsigned char)text[3]) ||
      !isdigit((unsigned char)text[4])) {
    return false;
  }
  int hour = ((text[0] - '0') * 10) + (text[1] - '0');
  int minute = ((text[3] - '0') * 10) + (text[4] - '0');
  if (hour > 23 || minute > 59) {
    return false;
  }
  *minutes = (hour * 60) + minute;
  return true;
}

// Parses an INT, BOOL or ENUM value. Out-of-range integers are clamped when
// not strict. Returns false after reporting a rejected value.
static bool parse_number(parser_t *parser, const key_spec_t *spec,
                         const char *text, int *value) {
  if (spec->kind == KEY_ENUM) {
    for (int i = 0; spec->choices[i]; i++) {
      if (strcmp(text, spec->choices[i]) == 0) {
        *value = spec->min + i;
        return true;
      }
    }
    report(parser, true, spec->name, "unknown value");
    return false;
  }
  int parsed;
  if (!parse_int(text, &parsed)) {
    report(parser, true, spec->name, "expected an integer");
    return false;
  }
  if (spec->kind == KEY_BOOL && parsed != 0 && parsed != 1) {
    report(parser, true, spec->name, "expected 0 or 1");
    return false;
  }
  if (parsed < spec->min || parsed > spec->max) {
    char message[96];
    snprintf(message, sizeof(message), "outside %d-%d", spec->min, spec->max);
    report(parser, true, spec->name, message);
    if (parser->strict) {
      return false;
    }
    parsed = parsed < spec->min ? spec->min : spec->max;
  }
  *value = parsed;
  return true;
}

static bongocat_error_t append(char ***array, int *count, const char *text) {
  char *copy = strdup(text);
  char **grown =
      copy ? realloc((void *)*array, ((size_t)*count + 1) * sizeof(**array))
           : NULL;
  if (!grown) {
    free(copy);
    return BONGOCAT_ERROR_MEMORY;
  }
  grown[(*count)++] = copy;
  *array = grown;
  return BONGOCAT_SUCCESS;
}

static void free_strings(char ***array, int *count) {
  for (int i = 0; i < *count; i++) {
    free((*array)[i]);
  }
  free((void *)*array);
  *array = NULL;
  *count = 0;
}

static bongocat_error_t parse_monitors(config_t *config, char *value) {
  free_strings(&config->monitors, &config->num_monitors);
  char *saveptr = NULL;
  for (char *token = strtok_r(value, ",", &saveptr); token;
       token = strtok_r(NULL, ",", &saveptr)) {
    char *name = trim(token);
    if (*name && append(&config->monitors, &config->num_monitors, name) !=
                     BONGOCAT_SUCCESS) {
      return BONGOCAT_ERROR_MEMORY;
    }
  }
  return BONGOCAT_SUCCESS;
}

static bongocat_error_t set_global(parser_t *parser, config_t *config,
                                   const key_spec_t *spec, char *value) {
  int number;
  switch (spec->kind) {
  case KEY_INT:
  case KEY_BOOL:
  case KEY_ENUM:
    if (parse_number(parser, spec, value, &number)) {
      memcpy((char *)config + spec->offset, &number, sizeof(number));
    }
    return BONGOCAT_SUCCESS;
  case KEY_TIME:
    if (parse_time(value, &number)) {
      memcpy((char *)config + spec->offset, &number, sizeof(number));
    } else {
      report(parser, true, spec->name, "expected HH:MM");
    }
    return BONGOCAT_SUCCESS;
  case KEY_MONITORS:
    return parse_monitors(config, value);
  case KEY_DEVICE:
    if (strncmp(value, "/dev/input/", 11) != 0 || strstr(value, "..")) {
      report(parser, true, spec->name,
             "path must be inside /dev/input/ without '..'");
      return BONGOCAT_SUCCESS;
    }
    return append(&config->keyboard_devices, &config->num_keyboard_devices,
                  value);
  case KEY_NAME:
    return append(&config->keyboard_names, &config->num_keyboard_names, value);
  case KEY_OBSOLETE:
    report(parser, false, spec->name, "obsolete setting is ignored");
    return BONGOCAT_SUCCESS;
  }
  return BONGOCAT_SUCCESS;
}

static bongocat_error_t set_override(parser_t *parser, config_t *config,
                                     const char *monitor,
                                     const key_spec_t *spec,
                                     const char *value) {
  if (!per_monitor(spec)) {
    report(parser, true, spec->name, "not allowed in a monitor section");
    return BONGOCAT_SUCCESS;
  }
  int number;
  if (!parse_number(parser, spec, value, &number)) {
    return BONGOCAT_SUCCESS;
  }
  monitor_override_t *entry = NULL;
  for (size_t i = 0; i < config->num_overrides; i++) {
    if (strcmp(config->overrides[i].monitor, monitor) == 0) {
      entry = &config->overrides[i];
    }
  }
  if (!entry) {
    char *name = strdup(monitor);
    monitor_override_t *grown =
        name ? realloc(config->overrides,
                       (config->num_overrides + 1) * sizeof(*grown))
             : NULL;
    if (!grown) {
      free(name);
      return BONGOCAT_ERROR_MEMORY;
    }
    config->overrides = grown;
    entry = &grown[config->num_overrides++];
    *entry = (monitor_override_t){.monitor = name};
  }
  size_t offset = spec->offset - offsetof(config_t, look);
  memcpy((char *)&entry->values + offset, &number, sizeof(number));
  entry->set |= 1U << (offset / sizeof(int));
  return BONGOCAT_SUCCESS;
}

// Removes an inline comment: '#' at the start or after whitespace.
static void strip_comment(char *value) {
  for (char *p = value; *p; p++) {
    if (*p == '#' && (p == value || isspace((unsigned char)p[-1]))) {
      *p = '\0';
      return;
    }
  }
}

typedef enum {
  SECTION_GLOBAL,
  SECTION_MONITOR,
  SECTION_INVALID
} section_t;

// Returns the section a header line selects; monitor names are copied out.
static section_t parse_section(char *line, char *monitor, size_t size) {
  size_t length = strlen(line);
  if (length < 2 || line[length - 1] != ']') {
    return SECTION_INVALID;
  }
  line[length - 1] = '\0';
  char *name = trim(line + 1);
  if (strcmp(name, "global") == 0) {
    return SECTION_GLOBAL;
  }
  if (strncmp(name, "monitor:", 8) != 0) {
    return SECTION_INVALID;
  }
  name = trim(name + 8);
  if (!*name || strlen(name) >= size) {
    return SECTION_INVALID;
  }
  memcpy(monitor, name, strlen(name) + 1);
  return SECTION_MONITOR;
}

static bongocat_error_t parse_file(parser_t *parser, config_t *config,
                                   FILE *file) {
  char *buffer = NULL;
  size_t capacity = 0;
  char monitor[128] = "";
  section_t section = SECTION_GLOBAL;
  bongocat_error_t result = BONGOCAT_SUCCESS;
  while (result == BONGOCAT_SUCCESS && getline(&buffer, &capacity, file) >= 0) {
    parser->line++;
    char *line = trim(buffer);
    if (!*line || *line == '#' || *line == ';') {
      continue;
    }
    if (*line == '[') {
      section = parse_section(line, monitor, sizeof(monitor));
      if (section == SECTION_INVALID) {
        // Keys below a broken header must not land in the previous section.
        report(parser, true, NULL, "invalid section header; ignoring its keys");
      }
      continue;
    }
    char *equals = strchr(line, '=');
    if (!equals) {
      report(parser, true, NULL, "expected key=value");
      continue;
    }
    *equals = '\0';
    char *key = trim(line);
    char *value = equals + 1;
    strip_comment(value);
    value = trim(value);
    if (section == SECTION_INVALID) {
      continue;
    }
    const key_spec_t *spec = find_key(key);
    if (!spec) {
      report(parser, true, key, "unknown setting");
    } else if (section == SECTION_MONITOR) {
      result = set_override(parser, config, monitor, spec, value);
    } else {
      result = set_global(parser, config, spec, value);
    }
  }
  if (result == BONGOCAT_SUCCESS && ferror(file)) {
    result = BONGOCAT_ERROR_FILE_IO;
  }
  free(buffer);
  return result;
}

// =============================================================================
// PUBLIC API
// =============================================================================

void config_free(config_t *config) {
  for (size_t i = 0; i < config->num_overrides; i++) {
    free(config->overrides[i].monitor);
  }
  free(config->overrides);
  free_strings(&config->monitors, &config->num_monitors);
  free_strings(&config->keyboard_devices, &config->num_keyboard_devices);
  free_strings(&config->keyboard_names, &config->num_keyboard_names);
  *config = (config_t){0};
}

static void log_summary(const config_t *config) {
  const appearance_t *look = &config->look;
  bongocat_log_debug(
      "Config: cat %dpx at (%d,%d) %s, bar %dpx %s opacity %d, layer %s, "
      "fps %d, monitors %d",
      look->cat_height, look->cat_x_offset, look->cat_y_offset,
      ALIGNMENTS[look->cat_align - ALIGN_LEFT], look->overlay_height,
      POSITIONS[look->overlay_position], look->overlay_opacity,
      LAYERS[look->layer], config->fps, config->num_monitors);
}

bongocat_error_t config_load(config_t *config, const char *path, bool strict) {
  *config = DEFAULTS;
  parser_t parser = {.path = path, .strict = strict};
  bongocat_error_t result = BONGOCAT_SUCCESS;
  if (path) {
    FILE *file = fopen(path, "r");
    if (file) {
      result = parse_file(&parser, config, file);
      fclose(file);
    } else if (strict) {
      bongocat_log_error("%s: %s", path, strerror(errno));
      result = BONGOCAT_ERROR_FILE_IO;
    } else {
      bongocat_log_warning("%s: %s; using defaults", path, strerror(errno));
    }
  }
  if (result == BONGOCAT_SUCCESS && parser.failed) {
    result = BONGOCAT_ERROR_CONFIG;
  }
  if (result != BONGOCAT_SUCCESS) {
    config_free(config);
    return result;
  }
  if (config->enable_scheduled_sleep &&
      config->sleep_begin == config->sleep_end) {
    bongocat_log_warning("sleep_begin equals sleep_end; scheduled sleep off");
    config->enable_scheduled_sleep = 0;
  }
  bongocat_log_set_debug(config->enable_debug);
  log_summary(config);
  return BONGOCAT_SUCCESS;
}

appearance_t config_appearance(const config_t *config, const char *monitor) {
  appearance_t look = config->look;
  for (size_t i = 0; monitor && i < config->num_overrides; i++) {
    const monitor_override_t *entry = &config->overrides[i];
    if (strcmp(entry->monitor, monitor) != 0) {
      continue;
    }
    for (size_t field = 0; field < APPEARANCE_FIELDS; field++) {
      if (entry->set & (1U << field)) {
        size_t offset = field * sizeof(int);
        memcpy((char *)&look + offset, (const char *)&entry->values + offset,
               sizeof(int));
      }
    }
  }
  return look;
}

char *config_resolve_path(const char *explicit_path) {
  if (explicit_path) {
    return strdup(explicit_path);
  }
  char path[PATH_MAX];
  const char *xdg = getenv("XDG_CONFIG_HOME");
  const char *home = getenv("HOME");
  if (xdg && *xdg &&
      snprintf(path, sizeof(path), "%s/bongocat/bongocat.conf", xdg) <
          (int)sizeof(path) &&
      access(path, R_OK) == 0) {
    return strdup(path);
  }
  if (home && *home &&
      snprintf(path, sizeof(path), "%s/.config/bongocat/bongocat.conf", home) <
          (int)sizeof(path) &&
      access(path, R_OK) == 0) {
    return strdup(path);
  }
  if (access("bongocat.conf", R_OK) == 0) {
    return strdup("bongocat.conf");
  }
  return NULL;
}
