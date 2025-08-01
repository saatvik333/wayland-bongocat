#define _POSIX_C_SOURCE 199309L
#define STB_IMAGE_IMPLEMENTATION
#include "animation.h"
#include "wayland.h"
#include "input.h"
#include "memory.h"
#include "embedded_assets_bongocat.h"
#include "embedded_assets.h"
#include <time.h>
#include <stdlib.h>

// Animation globals

sprite_sheet_frame_t empty_sprite_sheet_frame = (sprite_sheet_frame_t){
    .width = 0,
    .height = 0,
    .channels = 4,
    .pixels = NULL,
};
animation_t anims[TOTAL_ANIMATIONS];
int anim_frame_index = 0;
pthread_mutex_t anim_lock = PTHREAD_MUTEX_INITIALIZER;

static config_t *current_config;
static pthread_t anim_thread;

typedef struct {
    int min_x;
    int min_y;
    int max_x;
    int max_y;
    int crop_width;
    int crop_height;
    int padded_width;
    int padded_height;
} get_cropped_sizes_result_t;
static get_cropped_sizes_result_t get_cropped_sizes(sprite_sheet_frame_t loaded_frame, int padding_x, int padding_y) {
    const int width = loaded_frame.width;
    const int height = loaded_frame.height;
    const int channels = loaded_frame.channels;
    const unsigned char *src_pixels = loaded_frame.pixels;

    /// @TODO: optimize poor man crop image
    int min_x = width;
    int min_y = height;
    int max_x = -1;
    int max_y = -1;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const unsigned char alpha = src_pixels[(y * width + x) * channels + 3];
            if (alpha > THRESHOLD_ALPHA) {
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        }
    }

    // fully transparent
    if (min_x > max_x || min_y > max_y) {
        min_x = min_y = max_x = max_y = 0;
    }

    int crop_width = max_x - min_x + 1;
    int crop_height = max_y - min_y + 1;
    int padded_width = crop_width + 2 * padding_x;
    int padded_height = crop_height + 2 * padding_y;

    return (get_cropped_sizes_result_t) {
        .min_x = min_x,
        .min_y = min_y,
        .max_x = max_x,
        .max_y = max_y,
        .crop_width = crop_width,
        .crop_height = crop_height,
        .padded_width = padded_width,
        .padded_height = padded_height,
    };
}

static sprite_sheet_frame_t copy_image_from_memory_cropped(sprite_sheet_frame_t loaded_frame,
                                                           int padding_x, int padding_y, bool invert_color,
                                                           const get_cropped_sizes_result_t* cropping_size) {
    sprite_sheet_frame_t ret = empty_sprite_sheet_frame;
    const int width = loaded_frame.width;
    const int height = loaded_frame.height;
    const int channels = loaded_frame.channels;
    const unsigned char *src_pixels = loaded_frame.pixels;

    get_cropped_sizes_result_t padding_sizes = get_cropped_sizes(loaded_frame, padding_x, padding_y);

    int start_x = 0;
    int start_y = 0;
    if (cropping_size) {
        // @FIXME: center sprite so frame is not "jumping"
    } else {
        // skip cropping, when copying
        padding_sizes.min_x = 0;
        padding_sizes.min_y = 0;
        padding_sizes.max_x = width;
        padding_sizes.max_y = height;
        padding_sizes.padded_width = width;
        padding_sizes.padded_height = height;
        padding_sizes.crop_width = width;
        padding_sizes.crop_height = height;
    }

    unsigned char *dest_pixels = (unsigned char *)bongocat_malloc(padding_sizes.padded_width * padding_sizes.padded_height * channels);
    if (!dest_pixels) {
        return ret;
    }
    memset(dest_pixels, 0, padding_sizes.padded_width * padding_sizes.padded_height * channels);

    /// @TODO: optimize cropped region copy
    for (int y = 0; y < padding_sizes.crop_height; y++) {
        for (int x = 0; x < padding_sizes.crop_width; x++) {
            const int src_x = padding_sizes.min_x + x;
            const int src_y = padding_sizes.min_y + y;
            const int dst_x = start_x + x;
            const int dst_y = start_y + y;

            const size_t src_pixel_index = (src_y * width + src_x) * channels;

            // skip frame
            if (dst_x < 0 || dst_x >= padding_sizes.padded_width ||
                dst_y < 0 || dst_y >= padding_sizes.padded_height) {
                continue; // Skip out-of-bounds
            }

            const size_t dst_pixel_index = (dst_y * padding_sizes.padded_width + dst_x) * channels;

            if (src_pixel_index >= width * height * channels ||
                dst_pixel_index >= padding_sizes.padded_width * padding_sizes.padded_height * channels) {
                continue;
            }

            const unsigned char *src_pixel = &src_pixels[src_pixel_index];
            unsigned char *dst_pixel = &dest_pixels[dst_pixel_index];

            if (invert_color) {
                dst_pixel[0] = 255 - src_pixel[0]; //
                dst_pixel[1] = 255 - src_pixel[1]; //
                dst_pixel[2] = 255 - src_pixel[2]; //
                dst_pixel[3] = src_pixel[3];       // A (unchanged)
            } else {
                memcpy(dst_pixel, src_pixel, channels);
            }
        }
    }

    // move frame
    ret = loaded_frame;
    ret.width = padding_sizes.padded_width;
    ret.height = padding_sizes.padded_height;
    ret.pixels = dest_pixels;
    dest_pixels = NULL;

    return ret;
}
static get_cropped_sizes_result_t get_biggest_cropped_size(
    int frame_width,
    int frame_height,
    get_cropped_sizes_result_t* cropped_frames,
    size_t cropped_frames_size,
    int padding_x,
    int padding_y
) {
    get_cropped_sizes_result_t ret = {
        .min_x = frame_width,
        .min_y = frame_height,
        .max_x = 0,
        .max_y = 0,
        .crop_width = 0,
        .crop_height = 0,
        .padded_width = 0,
        .padded_height = 0,
    };

    for (size_t i = 0; i < cropped_frames_size; ++i) {
        get_cropped_sizes_result_t* crop = &cropped_frames[i];

        if (crop->min_x < ret.min_x) ret.min_x = crop->min_x;
        if (crop->min_y < ret.min_y) ret.min_y = crop->min_y;
        if (crop->max_x > ret.max_x) ret.max_x = crop->max_x;
        if (crop->max_y > ret.max_y) ret.max_y = crop->max_y;
    }

    ret.crop_width = ret.max_x - ret.min_x + 1;
    ret.crop_height = ret.max_y - ret.min_y + 1;
    ret.padded_width = ret.crop_width + 2 * padding_x;
    ret.padded_height = ret.crop_height + 2 * padding_y;

    return ret;
}

sprite_sheet_frame_t* load_sprite_sheet_from_memory(const uint8_t* sprite_data, int sprite_size,
                                                    int frame_columns, int frame_rows,
                                                    int* out_frame_count,
                                                    int padding_x, int padding_y, bool invert_color) {
    int sheet_width, sheet_height, channels;
    uint8_t* sprite_sheet_pixels = stbi_load_from_memory(sprite_data, sprite_size, &sheet_width, &sheet_height, &channels, 4); // Force RGBA
    if (!sprite_sheet_pixels) {
        bongocat_log_error("Failed to load sprite sheet.\n");
        return NULL;
    }

    if (frame_columns == 0 || frame_rows == 0 ||
        sheet_width % frame_columns != 0 || sheet_height % frame_rows != 0) {
        bongocat_log_error("Sprite sheet dimensions not divisible by frame grid.\n");
        stbi_image_free(sprite_sheet_pixels);
        sprite_sheet_pixels = NULL;
        return NULL;
    }

    int frame_width = sheet_width / frame_columns;
    int frame_height = sheet_height / frame_rows;
    const int total_frames = frame_columns * frame_rows;

    sprite_sheet_frame_t* frames = bongocat_malloc(sizeof(sprite_sheet_frame_t) * total_frames);
    if (!frames) {
        stbi_image_free(sprite_sheet_pixels);
        sprite_sheet_pixels = NULL;
        return NULL;
    }

    get_cropped_sizes_result_t* cropped_frames = bongocat_malloc(sizeof(get_cropped_sizes_result_t) * total_frames);
    /// @TODO: optimize search for the biggest padding
    for (int row = 0; row < frame_rows; ++row) {
        for (int col = 0; col < frame_columns; ++col) {
            const int idx = row * frame_columns + col;

            uint8_t* frame_pixels = bongocat_malloc(frame_width * frame_height * 4);
            if (!frame_pixels) {
                continue;
            }

            for (int y = 0; y < frame_height; ++y) {
                memcpy(
                    frame_pixels + y * frame_width * 4,
                    sprite_sheet_pixels + ((row * frame_height + y) * sheet_width + (col * frame_width)) * 4,
                    frame_width * 4
                );
            }

            const sprite_sheet_frame_t frame = (sprite_sheet_frame_t){
                .width = frame_width,
                .height = frame_height,
                .channels = 4,
                .pixels = frame_pixels
            };
            get_cropped_sizes_result_t cropping_result = get_cropped_sizes(frame, padding_x, padding_y);
            bongocat_log_debug("Cropped Sprite Frame (%d): %dx%d (%dx%d)", idx, cropping_result.padded_width, cropping_result.padded_height, frame_width, frame_height);

            cropped_frames[idx] = cropping_result;

            bongocat_free(frame_pixels);
            frame_pixels = NULL;
        }
    }
    const get_cropped_sizes_result_t cropping_size = get_biggest_cropped_size(frame_width,frame_height, cropped_frames, total_frames, padding_x, padding_y);
    bongocat_free(cropped_frames);

    for (int row = 0; row < frame_rows; ++row) {
        for (int col = 0; col < frame_columns; ++col) {
            const int idx = row * frame_columns + col;

            uint8_t* frame_pixels = bongocat_malloc(frame_width * frame_height * 4);
            if (!frame_pixels) {
                bongocat_log_error("Failed to allocate memory for frame %d\n", idx);
                // Cleanup previously allocated
                for (int j = 0; j < idx; ++j) {
                    if (frames[j].pixels) bongocat_free(frames[j].pixels);
                    frames[j].pixels = NULL;
                }
                bongocat_free(frames);
                return NULL;
            }

            for (int y = 0; y < frame_height; ++y) {
                memcpy(
                    frame_pixels + y * frame_width * 4,
                    sprite_sheet_pixels + ((row * frame_height + y) * sheet_width + (col * frame_width)) * 4,
                    frame_width * 4
                );
            }

            const sprite_sheet_frame_t frame = (sprite_sheet_frame_t){
                .width = frame_width,
                .height = frame_height,
                .channels = 4,
                .pixels = frame_pixels
            };
            frames[idx] = copy_image_from_memory_cropped(frame, padding_x, padding_y, invert_color, &cropping_size);
            bongocat_log_debug("Cropped Sprite Frame (%d): %dx%d (%dx%d)", idx, frames[idx].width, frames[idx].height, frame_width, frame_height);
            // frame_pixels moved
            bongocat_free(frame_pixels);
            frame_pixels = NULL;
        }
    }

    stbi_image_free(sprite_sheet_pixels);
    sprite_sheet_pixels = NULL;
    if (out_frame_count) *out_frame_count = total_frames;
    return frames;
}

void free_frames(sprite_sheet_frame_t* frames, int frame_count) {
    for (int i = 0; i < frame_count; ++i) {
        if (frames[i].pixels) bongocat_free(frames[i].pixels);
    }
    bongocat_free(frames);
}


void blit_image_scaled(uint8_t *dest, int dest_w, int dest_h,
                       unsigned char *src, int src_w, int src_h,
                       int offset_x, int offset_y, int target_w, int target_h) {
    for (int y = 0; y < target_h; y++) {
        for (int x = 0; x < target_w; x++) {
            int dx = x + offset_x;
            int dy = y + offset_y;
            if (dx < 0 || dy < 0 || dx >= dest_w || dy >= dest_h)
                continue;

            // Map destination pixel to source pixel
            int sx = (x * src_w) / target_w;
            int sy = (y * src_h) / target_h;

            int di = (dy * dest_w + dx) * 4;
            int si = (sy * src_w + sx) * 4;

            // Only draw non-transparent pixels
            if (src[si + 3] > THRESHOLD_ALPHA) {
                dest[di + 0] = src[si + 2]; // B
                dest[di + 1] = src[si + 1]; // G
                dest[di + 2] = src[si + 0]; // R
                dest[di + 3] = src[si + 3]; // A
            }
        }
    }
}

void draw_rect(uint8_t *dest, int width, int height, int x, int y, int w, int h, 
               uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            if (i < 0 || j < 0 || i >= width || j >= height)
                continue;
            int idx = (j * width + i) * 4;
            dest[idx + 0] = b;
            dest[idx + 1] = g;
            dest[idx + 2] = r;
            dest[idx + 3] = a;
        }
    }
}

static void *animate(void *arg __attribute__((unused))) {
    // Calculate frame time based on configured FPS
    long frame_time_ns = 1000000000L / current_config->fps;
    struct timespec ts = {0, frame_time_ns};
    
    long hold_until = 0;
    int test_counter = 0;
    int test_interval_frames = current_config->test_animation_interval * current_config->fps;

    while (1) {
        struct timeval now;
        gettimeofday(&now, NULL);
        long now_us = now.tv_sec * 1000000 + now.tv_usec;

        pthread_mutex_lock(&anim_lock);

        // Test animation based on configured interval
        if (current_config->test_animation_interval > 0) {
            test_counter++;
            if (test_counter > test_interval_frames) {
                if (current_config->enable_debug) {
                    printf("Test animation trigger\n");
                }
                anim_frame_index = (rand() % 2) + 1;
                hold_until = now_us + (current_config->test_animation_duration * 1000);
                test_counter = 0;
            }
        }

        if (*any_key_pressed) {
            if (current_config->enable_debug) {
                printf("Key pressed! Switching to frame %d\n", (rand() % 2) + 1);
            }
            anim_frame_index = (rand() % 2) + 1;
            hold_until = now_us + (current_config->keypress_duration * 1000);
            *any_key_pressed = 0;
            test_counter = 0; // Reset test counter
        } else if (now_us > hold_until) {
            if (anim_frame_index != current_config->idle_frame) {
                if (current_config->enable_debug) {
                    printf("Returning to idle frame %d\n", current_config->idle_frame);
                }
                anim_frame_index = current_config->idle_frame;
            }
        }
        
        pthread_mutex_unlock(&anim_lock);

        draw_bar();
        nanosleep(&ts, NULL);
    }
    
    return NULL;
}

bongocat_error_t animation_init(config_t *config) {
    BONGOCAT_CHECK_NULL(config, BONGOCAT_ERROR_INVALID_PARAM);
    
    current_config = config;
    
    bongocat_log_info("Initializing animation system");

    // empty animations
    for (size_t i = 0;i < TOTAL_ANIMATIONS; i++) {
        for (size_t j = 0; j < MAX_DIGIMON_FRAMES; j++) {
            anims[i].frames[j] = empty_sprite_sheet_frame;
        }
    }


    /// @TODO: get padding from config
    int padding_x = 4;  // or set as needed
    int padding_y = 4;  // or set as needed

    {
        // Load embedded animation frames (bongocat)
        const unsigned char *embedded_data[] = {
            bongo_cat_both_up_png,
            bongo_cat_left_down_png,
            bongo_cat_right_down_png,
            bongo_cat_both_down_png
        };

        const size_t embedded_sizes[] = {
            bongo_cat_both_up_png_size,
            bongo_cat_left_down_png_size,
            bongo_cat_right_down_png_size,
            bongo_cat_both_down_png_size
        };
        assert(LEN_ARRAY(embedded_data) <= BONGOCAT_NUM_FRAMES);
        assert(LEN_ARRAY(embedded_sizes) <= BONGOCAT_NUM_FRAMES);
        assert(LEN_ARRAY(embedded_sizes) == LEN_ARRAY(embedded_data));

        const char *frame_names[] = {
            "embedded bongo-cat-both-up.png",
            "embedded bongo-cat-left-down.png",
            "embedded bongo-cat-right-down.png",
            "embedded bongo-cat-both-down.png"
        };
        assert(LEN_ARRAY(frame_names) == LEN_ARRAY(embedded_data));

        sprite_sheet_frame_t bongocat_anims[BONGOCAT_NUM_FRAMES];
        sprite_sheet_frame_t loaded_frames[BONGOCAT_NUM_FRAMES];
        get_cropped_sizes_result_t cropped_frames[BONGOCAT_NUM_FRAMES];
        for (int i = 0; i < BONGOCAT_NUM_FRAMES; i++) {
            bongocat_log_debug("Loading embedded image: %s", frame_names[i]);

            loaded_frames[i].channels = 4;
            loaded_frames[i].pixels = stbi_load_from_memory(embedded_data[i], embedded_sizes[i],
                                                             &loaded_frames[i].width,
                                                             &loaded_frames[i].height,
                                                             NULL, loaded_frames[i].channels);
            if (!loaded_frames[i].pixels) {
                bongocat_log_error("Failed to load embedded image: %s", frame_names[i]);

                // Cleanup already loaded images
                for (int j = 0; j < i; j++) {
                    if (bongocat_anims[j].pixels) {
                        stbi_image_free(bongocat_anims[j].pixels);
                        bongocat_anims[j].pixels = NULL;
                    }
                }
                return BONGOCAT_ERROR_FILE_IO;
            }
            bongocat_log_debug("Loaded %dx%d embedded image", loaded_frames[i].width, loaded_frames[i].height);

            // detect croppable area
            cropped_frames[i] = get_cropped_sizes(loaded_frames[i], padding_x, padding_y);
        }
        /// @NOTE: assume every frame has the same dimentions
        const get_cropped_sizes_result_t cropping_size = get_biggest_cropped_size(loaded_frames[0].width, loaded_frames[0].height, cropped_frames, BONGOCAT_NUM_FRAMES, padding_x, padding_y);

        for (int i = 0; i < BONGOCAT_NUM_FRAMES; i++) {
            // skip cropping and padding for bongocat
            bongocat_anims[i] = copy_image_from_memory_cropped(loaded_frames[i], 0, 0, current_config->invert_color, NULL /*&cropping_size*/);
            // move loaded frame
            bongocat_log_debug("Loaded, cropped, padded: %dx%d -> %dx%d (padding %d,%d): %s",
                               loaded_frames[i].width, loaded_frames[i].height,
                               bongocat_anims[i].width, bongocat_anims[i].height,
                               0, 0,
                               frame_names[i]);
        }

        // clean up frames
        for (int i = 0; i < BONGOCAT_NUM_FRAMES; i++) {
            stbi_image_free(loaded_frames[i].pixels);
            loaded_frames[i].pixels = NULL;
        }

        // move frames into global anims
        anims[BONGOCAT_ANIM_INDEX].bongocat.both_up = bongocat_anims[0];
        anims[BONGOCAT_ANIM_INDEX].bongocat.left_down = bongocat_anims[1];
        anims[BONGOCAT_ANIM_INDEX].bongocat.right_down = bongocat_anims[2];
        anims[BONGOCAT_ANIM_INDEX].bongocat.both_down = bongocat_anims[3];
        for (int i = 0; i < BONGOCAT_NUM_FRAMES; i++) {
            bongocat_anims[i].pixels = NULL;
        }
    }

    /// @TODO: make macro
    // Load Agumon
    {
        int sprite_sheet_count = 0;
        assert(dm20_agumon_png_size <= INT_MAX);
        sprite_sheet_frame_t* sprite_sheet = load_sprite_sheet_from_memory(dm20_agumon_png,
                                                                           (int)dm20_agumon_png_size,
                                                                           DM20_AGUMON_SPRITE_SHEET_COLS,
                                                                           DM20_AGUMON_SPRITE_SHEET_ROWS,
                                                                           &sprite_sheet_count, padding_x, padding_y, current_config->invert_color);

        assert(sprite_sheet_count > 0);

        bongocat_log_debug("Loaded %dx%d sprite sheet with %d frames", sprite_sheet[0].width, sprite_sheet[0].height, sprite_sheet_count);

        if (sprite_sheet_count <= 4) {
            anims[DM20_AGUMON_ANIM_INDEX].digimon.idle_1 = sprite_sheet[DIGIMON_FRAME_IDLE1];
            anims[DM20_AGUMON_ANIM_INDEX].digimon.idle_2 = sprite_sheet[DIGIMON_FRAME_IDLE2];
            anims[DM20_AGUMON_ANIM_INDEX].digimon.angry = sprite_sheet[DIGIMON_FRAME_ANGRY];
            anims[DM20_AGUMON_ANIM_INDEX].digimon.down1 = sprite_sheet[DIGIMON_FRAME_DOWN1];

            // assign aliases/fallback
            /// @TODO: make copies of fallback frames ? vs. double free
            /*
            anims[DM20_AGUMON_ANIM_INDEX].digimon.happy = sprite_sheet[DIGIMON_FRAME_IDLE1];
            anims[DM20_AGUMON_ANIM_INDEX].digimon.eat1 = sprite_sheet[DIGIMON_FRAME_ANGRY];
            anims[DM20_AGUMON_ANIM_INDEX].digimon.sleep1 = sprite_sheet[DIGIMON_FRAME_DOWN1];
            anims[DM20_AGUMON_ANIM_INDEX].digimon.refuse = sprite_sheet[DIGIMON_FRAME_DOWN1];
            anims[DM20_AGUMON_ANIM_INDEX].digimon.sad = sprite_sheet[DIGIMON_FRAME_IDLE2];
            */

            // data has been moved
            sprite_sheet[DIGIMON_FRAME_IDLE1].pixels = NULL;
            sprite_sheet[DIGIMON_FRAME_IDLE2].pixels = NULL;
            sprite_sheet[DIGIMON_FRAME_ANGRY].pixels = NULL;
            sprite_sheet[DIGIMON_FRAME_DOWN1].pixels = NULL;
        } else if (sprite_sheet_count <= 9) {
            anims[DM20_AGUMON_ANIM_INDEX].digimon.idle_1 = sprite_sheet[DIGIMON_FRAME_IDLE1];
            anims[DM20_AGUMON_ANIM_INDEX].digimon.idle_2 = sprite_sheet[DIGIMON_FRAME_IDLE2];
            anims[DM20_AGUMON_ANIM_INDEX].digimon.angry = sprite_sheet[DIGIMON_FRAME_ANGRY];
            anims[DM20_AGUMON_ANIM_INDEX].digimon.down1 = sprite_sheet[DIGIMON_FRAME_DOWN1];

            anims[DM20_AGUMON_ANIM_INDEX].digimon.happy = sprite_sheet[DIGIMON_FRAME_HAPPY];
            anims[DM20_AGUMON_ANIM_INDEX].digimon.eat1 = sprite_sheet[DIGIMON_FRAME_EAT1];
            anims[DM20_AGUMON_ANIM_INDEX].digimon.sleep1 = sprite_sheet[DIGIMON_FRAME_SLEEP1];
            anims[DM20_AGUMON_ANIM_INDEX].digimon.refuse = sprite_sheet[DIGIMON_FRAME_REFUSE];
            anims[DM20_AGUMON_ANIM_INDEX].digimon.sad = sprite_sheet[DIGIMON_FRAME_SAD];

            // data has been moved
            sprite_sheet[DIGIMON_FRAME_IDLE1].pixels = NULL;
            sprite_sheet[DIGIMON_FRAME_IDLE2].pixels = NULL;
            sprite_sheet[DIGIMON_FRAME_ANGRY].pixels = NULL;
            sprite_sheet[DIGIMON_FRAME_DOWN1].pixels = NULL;
            sprite_sheet[DIGIMON_FRAME_HAPPY].pixels = NULL;
            sprite_sheet[DIGIMON_FRAME_EAT1].pixels = NULL;
            sprite_sheet[DIGIMON_FRAME_ANGRY].pixels = NULL;
            sprite_sheet[DIGIMON_FRAME_SLEEP1].pixels = NULL;
            sprite_sheet[DIGIMON_FRAME_REFUSE].pixels = NULL;
            sprite_sheet[DIGIMON_FRAME_SAD].pixels = NULL;
        } else if (sprite_sheet_count <= 13) {
            anims[DM20_AGUMON_ANIM_INDEX].digimon.idle_1 = sprite_sheet[DIGIMON_FRAME_IDLE1];
            anims[DM20_AGUMON_ANIM_INDEX].digimon.idle_2 = sprite_sheet[DIGIMON_FRAME_IDLE2];
            anims[DM20_AGUMON_ANIM_INDEX].digimon.angry = sprite_sheet[DIGIMON_FRAME_ANGRY];
            anims[DM20_AGUMON_ANIM_INDEX].digimon.down1 = sprite_sheet[DIGIMON_FRAME_DOWN1];

            anims[DM20_AGUMON_ANIM_INDEX].digimon.happy = sprite_sheet[DIGIMON_FRAME_HAPPY];
            anims[DM20_AGUMON_ANIM_INDEX].digimon.eat1 = sprite_sheet[DIGIMON_FRAME_EAT1];
            anims[DM20_AGUMON_ANIM_INDEX].digimon.sleep1 = sprite_sheet[DIGIMON_FRAME_SLEEP1];
            anims[DM20_AGUMON_ANIM_INDEX].digimon.refuse = sprite_sheet[DIGIMON_FRAME_REFUSE];
            anims[DM20_AGUMON_ANIM_INDEX].digimon.sad = sprite_sheet[DIGIMON_FRAME_DOWN2];

            anims[DM20_AGUMON_ANIM_INDEX].digimon.down_2 = sprite_sheet[DIGIMON_FRAME_DOWN2];
            anims[DM20_AGUMON_ANIM_INDEX].digimon.eat_2 = sprite_sheet[DIGIMON_FRAME_EAT2];
            anims[DM20_AGUMON_ANIM_INDEX].digimon.sleep_2 = sprite_sheet[DIGIMON_FRAME_SLEEP2];
            anims[DM20_AGUMON_ANIM_INDEX].digimon.attack = sprite_sheet[DIGIMON_FRAME_ATTACK];

            // data has been moved
            sprite_sheet[DIGIMON_FRAME_IDLE1].pixels = NULL;
            sprite_sheet[DIGIMON_FRAME_IDLE2].pixels = NULL;
            sprite_sheet[DIGIMON_FRAME_ANGRY].pixels = NULL;
            sprite_sheet[DIGIMON_FRAME_DOWN1].pixels = NULL;
            sprite_sheet[DIGIMON_FRAME_HAPPY].pixels = NULL;
            sprite_sheet[DIGIMON_FRAME_EAT1].pixels = NULL;
            sprite_sheet[DIGIMON_FRAME_ANGRY].pixels = NULL;
            sprite_sheet[DIGIMON_FRAME_SLEEP1].pixels = NULL;
            sprite_sheet[DIGIMON_FRAME_REFUSE].pixels = NULL;
            sprite_sheet[DIGIMON_FRAME_SAD].pixels = NULL;
            sprite_sheet[DIGIMON_FRAME_DOWN2].pixels = NULL;
            sprite_sheet[DIGIMON_FRAME_EAT2].pixels = NULL;
            sprite_sheet[DIGIMON_FRAME_SLEEP2].pixels = NULL;
            sprite_sheet[DIGIMON_FRAME_ATTACK].pixels = NULL;
        } else {
            bongocat_log_error("Sprite Sheet must at least have 4. frames but has '%i'", sprite_sheet_count);
        }

        free_frames(sprite_sheet, sprite_sheet_count);
    }
    
    bongocat_log_info("Animation system initialized successfully with embedded assets");
    return BONGOCAT_SUCCESS;
}

bongocat_error_t animation_start(void) {
    bongocat_log_info("Starting animation thread");
    
    int result = pthread_create(&anim_thread, NULL, animate, NULL);
    if (result != 0) {
        bongocat_log_error("Failed to create animation thread: %s", strerror(result));
        return BONGOCAT_ERROR_THREAD;
    }
    
    bongocat_log_debug("Animation thread started successfully");
    return BONGOCAT_SUCCESS;
}

void animation_cleanup(void) {
    pthread_cancel(anim_thread);
    pthread_join(anim_thread, NULL);
    
    for (int i = 0; i < TOTAL_ANIMATIONS; i++) {
        for (int j = 0; j < MAX_DIGIMON_FRAMES; j++) {
            if (anims[i].frames[j].width) {
                if (anims[i].frames[j].pixels) stbi_image_free(anims[i].frames[j].pixels);
                anims[i].frames[j].pixels = NULL;
            }
        }
    }
    
    pthread_mutex_destroy(&anim_lock);
}

void animation_trigger(void) {
    *any_key_pressed = 1;
}