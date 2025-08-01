#ifndef CONFIG_H
#define CONFIG_H

#include "bongocat.h"
#include "error.h"

typedef enum {
    POSITION_TOP = 0,
    POSITION_BOTTOM = 1
} overlay_position_t;

typedef struct {
    int screen_width;
    int bar_height;
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
    int enable_debug;
    overlay_position_t overlay_position;
    int animation_index;
    int invert_color;
} config_t;

bongocat_error_t load_config(config_t *config, const char *config_file_path);
void config_cleanup(void);
int get_screen_width(void);

#endif // CONFIG_H