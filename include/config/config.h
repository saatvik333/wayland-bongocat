#ifndef CONFIG_H
#define CONFIG_H

#include "core/bongocat.h"
#include "utils/error.h"

#include <stdbool.h>

// =============================================================================
// CONFIGURATION ENUMS
// =============================================================================

typedef enum {
  POSITION_TOP = 0,
  POSITION_BOTTOM = 1
} overlay_position_t;

typedef enum {
  LAYER_TOP = 0,
  LAYER_OVERLAY = 1
} layer_type_t;

typedef enum {
  ALIGN_LEFT = -1,
  ALIGN_CENTER = 0,
  ALIGN_RIGHT = 1,
} align_type_t;

// =============================================================================
// CONFIGURATION TYPES
// =============================================================================

typedef struct {
  int hour;
  int min;
} config_time_t;

typedef struct {
  // Display settings
  int screen_width;
  char *output_name;
  int bar_height;
  int overlay_height;
  int overlay_opacity;
  layer_type_t layer;
  overlay_position_t overlay_position;

  // Cat appearance
  const char *asset_paths[NUM_FRAMES];
  int cat_x_offset;
  int cat_y_offset;
  int cat_height;
  int mirror_x;             // Reflect across Y axis (horizontal flip)
  int mirror_y;             // Reflect across X axis (vertical flip)
  int enable_antialiasing;  // Enable bilinear interpolation
  align_type_t cat_align;

  // Animation timing
  int idle_frame;
  int keypress_duration;
  int test_animation_duration;
  int test_animation_interval;
  int fps;
  int enable_hand_mapping;  // 0=random hands, 1=based on key position

  // Input devices
  char **keyboard_devices;
  int num_keyboard_devices;
  int hotplug_scan_interval;

  // Device matching criteria (for auto-detection/hotplug)
  char **keyboard_names;
  int num_names;

  // Sleep schedule
  int enable_scheduled_sleep;
  config_time_t sleep_begin;
  config_time_t sleep_end;
  int idle_sleep_timeout_sec;

  // Debug
  int enable_debug;
} config_t;

// =============================================================================
// CONFIGURATION FUNCTIONS
// =============================================================================

// Load configuration - returns error code (must be checked)
BONGOCAT_NODISCARD bongocat_error_t load_config(config_t *config,
                                                const char *config_file_path);

// Get screen width - returns 0 on failure (should be checked)
BONGOCAT_NODISCARD int get_screen_width(void);

// Cleanup functions
void config_cleanup(void);
void config_cleanup_full(config_t *config);

#endif  // CONFIG_H