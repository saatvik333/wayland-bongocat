#ifndef CONFIG_H
#define CONFIG_H

#include "utils/error.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
  POSITION_TOP,
  POSITION_BOTTOM
} overlay_position_t;
typedef enum {
  LAYER_BACKGROUND,
  LAYER_BOTTOM,
  LAYER_TOP,
  LAYER_OVERLAY
} layer_type_t;
typedef enum {
  ALIGN_LEFT = -1,
  ALIGN_CENTER,
  ALIGN_RIGHT
} align_type_t;

// Settings a [monitor:NAME] section may override. Every field is an int so
// overrides can be applied field by field.
typedef struct {
  int cat_height;
  int overlay_height;
  int overlay_opacity;
  int cat_x_offset;
  int cat_y_offset;
  int layer;             // layer_type_t
  int overlay_position;  // overlay_position_t
  int cat_align;         // align_type_t
  int mirror_x;          // Reflect across the vertical axis
  int mirror_y;          // Reflect across the horizontal axis
  int disable_fullscreen_hide;
} appearance_t;

typedef struct {
  char *monitor;
  appearance_t values;
  unsigned set;  // Bit i: the i-th appearance_t field is overridden
} monitor_override_t;

typedef struct {
  appearance_t look;
  monitor_override_t *overrides;
  size_t num_overrides;

  char **monitors;  // Explicit output names; empty means automatic
  int num_monitors;
  int follow_active_monitor;

  char **keyboard_devices;
  int num_keyboard_devices;
  char **keyboard_names;
  int num_keyboard_names;

  int fps;
  int keypress_duration;        // ms
  int idle_frame;               // Frame index shown while idle
  int test_animation_duration;  // ms
  int test_animation_interval;  // s, 0 disables
  int enable_hand_mapping;      // 0 picks a random paw per keypress

  int enable_scheduled_sleep;
  int sleep_begin;         // Minutes after midnight
  int sleep_end;           // Minutes after midnight
  int idle_sleep_timeout;  // s, 0 disables

  int enable_debug;
} config_t;

// Loads defaults, then path if non-NULL. Strict loading fails on any invalid
// line or missing file; otherwise problems are logged and the line ignored.
// On failure the config is left empty and owns no memory.
[[nodiscard]] bongocat_error_t config_load(config_t *config, const char *path,
                                           bool strict);
void config_free(config_t *config);

// Global appearance with the overrides for monitor applied.
appearance_t config_appearance(const config_t *config, const char *monitor);

// Explicit path, else $XDG_CONFIG_HOME/bongocat/bongocat.conf,
// ~/.config/bongocat/bongocat.conf or ./bongocat.conf. NULL when none exists.
// The caller frees the result.
char *config_resolve_path(const char *explicit_path);

#endif  // CONFIG_H
