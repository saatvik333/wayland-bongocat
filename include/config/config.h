#ifndef CONFIG_H
#define CONFIG_H

#include "core/bongocat.h"
#include "utils/error.h"

typedef enum {
    POSITION_TOP = 0,
    POSITION_BOTTOM = 1
} overlay_position_t;

typedef enum {
    LAYER_TOP = 0,
    LAYER_OVERLAY = 1
} layer_type_t;

typedef struct {
    int hour;
    int min;
} config_time_t;

typedef enum {
    ALIGN_LEFT = -1,
    ALIGN_CENTER = 0,
    ALIGN_RIGHT = 1,
} align_type_t;

typedef struct {
    int screen_width;
    char *output_name;
    int bar_height;
    const char *asset_paths[NUM_FRAMES];
    char **keyboard_devices;
    int num_keyboard_devices;
    int cat_x_offset;
    int cat_y_offset;
    int cat_height;
    int overlay_height;
    int idle_frame;
    int keypress_duration;
    int test_animation_duration;
    int test_animation_interval;
    int fps;
    int overlay_opacity;
    int mirror_x; // reflect across Y axis (horizontal flip)
    int mirror_y; // reflect across X axis (vertical flip)
    int enable_debug;
    layer_type_t layer;
    overlay_position_t overlay_position;

    int enable_scheduled_sleep;
    config_time_t sleep_begin;
    config_time_t sleep_end;
    int idle_sleep_timeout_sec;
    align_type_t cat_align;
} config_t;

bongocat_error_t load_config(config_t *config, const char *config_file_path);
void config_cleanup(void);
void config_cleanup_full(config_t *config);
int get_screen_width(void);

#endif // CONFIG_H