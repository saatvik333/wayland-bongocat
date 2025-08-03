#define _POSIX_C_SOURCE 199309L
#define STB_IMAGE_IMPLEMENTATION
#include "graphics/animation.h"
#include "platform/wayland.h"
#include "platform/input.h"
#include "utils/memory.h"
#include "graphics/embedded_assets_bongocat.h"
#include "graphics/embedded_assets.h"
#include "utils/time.h"
#include <time.h>
#include <stdlib.h>
#include <pthread.h>
#include "../lib/stb_image.h"

// =============================================================================
// GLOBAL STATE AND CONFIGURATION
// =============================================================================

#define THRESHOLD_ALPHA 127

// Bongocat Frames
#define BONGOCAT_FRAME_BOTH_DOWN 0
#define BONGOCAT_FRAME_LEFT_DOWN 1
#define BONGOCAT_FRAME_RIGHT_DOWN 2
#define BONGOCAT_FRAME_BOTH_UP 3

// Digimon (Sprite Sheet) Frames
#define DIGIMON_FRAME_IDLE1 0
#define DIGIMON_FRAME_IDLE2 1
#define DIGIMON_FRAME_ANGRY 2 // Angry/Refuse or Hit (Fallback), Eat Frame Fallback
#define DIGIMON_FRAME_DOWN1 3 // Sleep/Discipline Fallback
#define DIGIMON_FRAME_HAPPY 4
#define DIGIMON_FRAME_EAT1 5
#define DIGIMON_FRAME_SLEEP1 6
#define DIGIMON_FRAME_REFUSE 7
#define DIGIMON_FRAME_SAD 8

// optional frames
//#define DIGIMON_FRAME_DOWN2 9
//#define DIGIMON_FRAME_EAT2 10
//#define DIGIMON_FRAME_SLEEP2 11
//#define DIGIMON_FRAME_ATTACK 12

//#define DIGIMON_FRAME_MOVEMENT1 13
//#define DIGIMON_FRAME_MOVEMENT2 14

static const animation_frame_t empty_sprite_sheet_frame = (animation_frame_t){
    .width = 0,
    .height = 0,
    .channels = RGBA_CHANNELS,
    .pixels = NULL,
};

// =============================================================================
// DRAWING OPERATIONS MODULE
// =============================================================================

static bool drawing_is_pixel_in_bounds(int x, int y, int width, int height) {
    return (x >= 0 && y >= 0 && x < width && y < height);
}

typedef enum {
    COPY_PIXEL_OPTION_NORMAL,
    COPY_PIXEL_OPTION_INVERT,
} drawing_copy_pixel_color_option_t;
static void drawing_copy_pixel(uint8_t *dest, const unsigned char *src, int dest_idx, int src_idx, drawing_copy_pixel_color_option_t option) {
    switch (option) {
        case COPY_PIXEL_OPTION_NORMAL:
            dest[dest_idx + 0] = src[src_idx + 2]; // B
            dest[dest_idx + 1] = src[src_idx + 1]; // G
            dest[dest_idx + 2] = src[src_idx + 0]; // R
            dest[dest_idx + 3] = src[src_idx + 3]; // A
            break;
        case COPY_PIXEL_OPTION_INVERT:
            dest[dest_idx + 0] = 255 - src[src_idx + 2]; // B
            dest[dest_idx + 1] = 255 - src[src_idx + 1]; // G
            dest[dest_idx + 2] = 255 - src[src_idx + 0]; // R
            dest[dest_idx + 3] = src[src_idx + 3]; // A
            break;
    }
}


typedef struct {
    int min_x;
    int min_y;
    int max_x;
    int max_y;
    int crop_width;
    int crop_height;
    int padded_width;
    int padded_height;
} get_cropped_size_result_t;
static get_cropped_size_result_t get_cropped_size(animation_frame_t loaded_frame, int padding_x, int padding_y) {
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

    return (get_cropped_size_result_t) {
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

static bongocat_error_t copy_cropped_frame(animation_frame_t* out_frame,
                                           animation_frame_t in_frame,
                                           int padding_x, int padding_y, drawing_copy_pixel_color_option_t drawing_option,
                                           const get_cropped_size_result_t* cropping_size) {
    *out_frame = empty_sprite_sheet_frame;
    const int width = in_frame.width;
    const int height = in_frame.height;
    const int channels = in_frame.channels;
    const unsigned char *src_pixels = in_frame.pixels;

    get_cropped_size_result_t padding_sizes = get_cropped_size(in_frame, padding_x, padding_y);

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

    unsigned char *dest_pixels = BONGOCAT_MALLOC(padding_sizes.padded_width * padding_sizes.padded_height * channels);
    if (!dest_pixels) {
        return BONGOCAT_ERROR_MEMORY;
    }
    memset(dest_pixels, 0, padding_sizes.padded_width * padding_sizes.padded_height * channels);

    /// @TODO: optimize cropped region copy
    for (int y = 0; y < padding_sizes.crop_height; y++) {
        for (int x = 0; x < padding_sizes.crop_width; x++) {
            const int src_x = padding_sizes.min_x + x;
            const int src_y = padding_sizes.min_y + y;
            const int dst_x = start_x + x;
            const int dst_y = start_y + y;

            const int src_pixel_index = (src_y * width + src_x) * channels;

            // skip frame
            if (dst_x < 0 || dst_x >= padding_sizes.padded_width ||
                dst_y < 0 || dst_y >= padding_sizes.padded_height) {
                continue; // Skip out-of-bounds
            }

            const int dst_pixel_index = (dst_y * padding_sizes.padded_width + dst_x) * channels;

            if (src_pixel_index >= width * height * channels ||
                dst_pixel_index >= padding_sizes.padded_width * padding_sizes.padded_height * channels) {
                continue;
            }

            drawing_copy_pixel(dest_pixels, src_pixels, dst_pixel_index, src_pixel_index, drawing_option);
        }
    }

    // move frame
    *out_frame = in_frame;
    out_frame->width = padding_sizes.padded_width;
    out_frame->height = padding_sizes.padded_height;
    // move dest_pixels
    out_frame->pixels = dest_pixels;
    dest_pixels = NULL;

    return BONGOCAT_SUCCESS;
}
static get_cropped_size_result_t get_biggest_cropped_size(
    int frame_width,
    int frame_height,
    get_cropped_size_result_t* cropped_frames,
    size_t cropped_frames_size,
    int padding_x,
    int padding_y
) {
    get_cropped_size_result_t ret = {
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
        get_cropped_size_result_t* crop = &cropped_frames[i];

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

static bongocat_error_t load_sprite_sheet_from_memory(animation_frame_t* out_frames, size_t out_frames_count,
                                          const uint8_t* sprite_data, int sprite_size,
                                          int frame_columns, int frame_rows,
                                          int* out_frame_count,
                                          int padding_x, int padding_y,
                                          drawing_copy_pixel_color_option_t drawing_option) {
    int sheet_width, sheet_height, channels;
    uint8_t* sprite_sheet_pixels = stbi_load_from_memory(sprite_data, sprite_size, &sheet_width, &sheet_height, &channels, RGBA_CHANNELS); // Force RGBA
    if (!sprite_sheet_pixels) {
        bongocat_log_error("Failed to load sprite sheet.");
        return BONGOCAT_ERROR_FILE_IO;
    }

    if (frame_columns == 0 || frame_rows == 0 ||
        sheet_width % frame_columns != 0 || sheet_height % frame_rows != 0) {
        bongocat_log_error("Sprite sheet dimensions not divisible by frame grid.");
        stbi_image_free(sprite_sheet_pixels);
        sprite_sheet_pixels = NULL;
        return BONGOCAT_ERROR_INVALID_PARAM;
    }

    int frame_width = sheet_width / frame_columns;
    int frame_height = sheet_height / frame_rows;
    const int total_frames = frame_columns * frame_rows;

    if (total_frames > out_frames_count) {
        bongocat_log_error("Sprite Sheet does not fit in out_frames: %d, total_frames: %d", out_frames_count, total_frames);
        stbi_image_free(sprite_sheet_pixels);
        sprite_sheet_pixels = NULL;
        return BONGOCAT_ERROR_INVALID_PARAM;
    }

    get_cropped_size_result_t* cropped_frames = BONGOCAT_MALLOC(sizeof(get_cropped_size_result_t) * total_frames);
    /// @TODO: optimize search for the biggest padding
    for (int row = 0; row < frame_rows; ++row) {
        for (int col = 0; col < frame_columns; ++col) {
            const int idx = row * frame_columns + col;

            uint8_t* frame_pixels = BONGOCAT_MALLOC(frame_width * frame_height * RGBA_CHANNELS);
            if (!frame_pixels) {
                continue;
            }

            for (int y = 0; y < frame_height; ++y) {
                memcpy(
                    frame_pixels + y * frame_width * RGBA_CHANNELS,
                    sprite_sheet_pixels + ((row * frame_height + y) * sheet_width + (col * frame_width)) * RGBA_CHANNELS,
                    frame_width * RGBA_CHANNELS
                );
            }

            const animation_frame_t frame = (animation_frame_t){
                .width = frame_width,
                .height = frame_height,
                .channels = RGBA_CHANNELS,
                .pixels = frame_pixels
            };
            get_cropped_size_result_t cropping_result = get_cropped_size(frame, padding_x, padding_y);
            bongocat_log_debug("Cropped Sprite Frame (%d): %dx%d (%dx%d)", idx, cropping_result.padded_width, cropping_result.padded_height, frame_width, frame_height);

            cropped_frames[idx] = cropping_result;

            BONGOCAT_FREE(frame_pixels);
            frame_pixels = NULL;
        }
    }
    const get_cropped_size_result_t cropping_size = get_biggest_cropped_size(frame_width,frame_height, cropped_frames, total_frames, padding_x, padding_y);
    BONGOCAT_FREE(cropped_frames);

    for (int row = 0; row < frame_rows; ++row) {
        for (int col = 0; col < frame_columns; ++col) {
            const int idx = row * frame_columns + col;

            uint8_t* frame_pixels = BONGOCAT_MALLOC(frame_width * frame_height * RGBA_CHANNELS);
            if (!frame_pixels) {
                bongocat_log_error("Failed to allocate memory for frame %d\n", idx);
                // Cleanup previously allocated
                for (int j = 0; j < idx; ++j) {
                    if (out_frames[j].pixels) BONGOCAT_FREE(out_frames[j].pixels);
                    out_frames[j].pixels = NULL;
                }
                return BONGOCAT_ERROR_MEMORY;
            }

            for (int y = 0; y < frame_height; ++y) {
                memcpy(
                    frame_pixels + y * frame_width * RGBA_CHANNELS,
                    sprite_sheet_pixels + ((row * frame_height + y) * sheet_width + (col * frame_width)) * RGBA_CHANNELS,
                    frame_width * RGBA_CHANNELS
                );
            }

            const animation_frame_t frame_data = (animation_frame_t){
                .width = frame_width,
                .height = frame_height,
                .channels = RGBA_CHANNELS,
                .pixels = frame_pixels
            };

            copy_cropped_frame(&out_frames[idx], frame_data, padding_x, padding_y, drawing_option, &cropping_size);

            bongocat_log_debug("Cropped Sprite Frame (%d): %dx%d (%dx%d)", idx, out_frames[idx].width, out_frames[idx].height, frame_width, frame_height);

            BONGOCAT_FREE(frame_pixels);
            frame_pixels = NULL;
        }
    }

    stbi_image_free(sprite_sheet_pixels);
    sprite_sheet_pixels = NULL;

    if (out_frame_count) *out_frame_count = total_frames;

    return BONGOCAT_SUCCESS;
}

static void free_frames(animation_frame_t* frames, size_t frame_count) {
    for (size_t i = 0; i < frame_count; ++i) {
        BONGOCAT_SAFE_FREE(frames[i].pixels);
    }
    BONGOCAT_FREE(frames);
    frames = NULL;
}

void blit_image_scaled(uint8_t *dest, int dest_w, int dest_h,
                       const unsigned char *src, int src_w, int src_h,
                       int offset_x, int offset_y, int target_w, int target_h) {
    for (int y = 0; y < target_h; y++) {
        for (int x = 0; x < target_w; x++) {
            int dx = x + offset_x;
            int dy = y + offset_y;
            
            if (!drawing_is_pixel_in_bounds(dx, dy, dest_w, dest_h)) {
                continue;
            }

            // Map destination pixel to source pixel
            int sx = (x * src_w) / target_w;
            int sy = (y * src_h) / target_h;

            int dest_idx = (dy * dest_w + dx) * RGBA_CHANNELS;
            int src_idx = (sy * src_w + sx) * RGBA_CHANNELS;

            // Only draw non-transparent pixels
            if (src[src_idx + 3] > THRESHOLD_ALPHA) {
                drawing_copy_pixel(dest, src, dest_idx, src_idx, COPY_PIXEL_OPTION_NORMAL);
            }
        }
    }
}

void draw_rect(uint8_t *dest, int width, int height, int x, int y, int w, int h, 
               uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            if (!drawing_is_pixel_in_bounds(i, j, width, height)) {
                continue;
            }
            
            int idx = (j * width + i) * RGBA_CHANNELS;
            dest[idx + 0] = b;
            dest[idx + 1] = g;
            dest[idx + 2] = r;
            dest[idx + 3] = a;
        }
    }
}

// =============================================================================
// ANIMATION STATE MANAGEMENT MODULE
// =============================================================================

typedef struct {
    timestamp_us_t hold_until_us;
    int test_counter;
    int test_interval_frames;
    time_ns_t frame_time_ns;
} animation_state_t;

static int anim_get_random_active_frame(animation_context_t* ctx) {
    if (ctx->anim_index == BONGOCAT_ANIM_INDEX) {
        return (rand() % 2) + 1; // Frame 1 or 2 (active frames)
    }

    // toggle frame
    const int current_frame = ctx->anim_frame_index;
    if (current_frame == DIGIMON_FRAME_IDLE1) {
        return DIGIMON_FRAME_IDLE2;
    } else if (current_frame == DIGIMON_FRAME_IDLE2) {
        return DIGIMON_FRAME_IDLE1;
    }

    return rand() % 2; // Frame 0 or 1 (active frames)
}

static void anim_trigger_frame_change(animation_context_t* ctx,
                                      int new_frame, long duration_us, long current_time_us,
                                      animation_state_t *state) {
    if (ctx->_current_config->enable_debug) {
        bongocat_log_debug("Animation frame change: %d (duration: %ld us)", new_frame, duration_us);
    }
    
    ctx->anim_frame_index = new_frame;
    state->hold_until_us = current_time_us + duration_us;
}

static void anim_handle_test_animation(animation_context_t* ctx, animation_state_t *state, timestamp_us_t current_time_us) {
    if (ctx->_current_config->test_animation_interval <= 0) {
        return;
    }
    
    state->test_counter++;
    if (state->test_counter > state->test_interval_frames) {
        int new_frame = anim_get_random_active_frame(ctx);
        long duration_us = ctx->_current_config->test_animation_duration * 1000;
        
        bongocat_log_debug("Test animation trigger");
        anim_trigger_frame_change(ctx, new_frame, duration_us, current_time_us, state);
        state->test_counter = 0;
    }
}

static void anim_handle_key_press(animation_context_t* ctx, input_context_t *input, animation_state_t *state, timestamp_us_t current_time_us) {
    if (!*input->any_key_pressed) {
        return;
    }
    
    int new_frame = anim_get_random_active_frame(ctx);
    long duration_us = ctx->_current_config->keypress_duration * 1000;
    
    bongocat_log_debug("Key press detected - switching to frame %d", new_frame);
    anim_trigger_frame_change(ctx, new_frame, duration_us, current_time_us, state);
    
    *input->any_key_pressed = 0;
    state->test_counter = 0; // Reset test counter
}

static void anim_handle_idle_return(animation_context_t* ctx, animation_state_t *state, timestamp_us_t current_time_us) {
    if (current_time_us <= state->hold_until_us) {
        return;
    }
    
    if (ctx->anim_frame_index != ctx->_current_config->idle_frame) {
        bongocat_log_debug("Returning to idle frame %d", ctx->_current_config->idle_frame);
        ctx->anim_frame_index = ctx->_current_config->idle_frame;
    }
}

static void anim_update_state(animation_context_t* ctx, input_context_t *input, animation_state_t *state) {
    timestamp_us_t current_time_us = get_current_time_us();
    
    pthread_mutex_lock(&ctx->anim_lock);
    
    anim_handle_test_animation(ctx, state, current_time_us);
    anim_handle_key_press(ctx, input, state, current_time_us);
    anim_handle_idle_return(ctx, state, current_time_us);
    
    pthread_mutex_unlock(&ctx->anim_lock);
}

// =============================================================================
// ANIMATION THREAD MANAGEMENT MODULE
// =============================================================================

static void anim_init_state(animation_context_t* ctx, animation_state_t *state) {
    state->hold_until_us = 0;
    state->test_counter = 0;
    state->test_interval_frames = ctx->_current_config->test_animation_interval * ctx->_current_config->fps;
    state->frame_time_ns = 1000000000L / ctx->_current_config->fps;
}


typedef struct {
    input_context_t *input;
    animation_context_t* ctx;
    wayland_context_t* wayland;
} anim_thread_main_params_t;
static void *anim_thread_main(void *arg) {
    assert(arg);
    anim_thread_main_params_t* animate_params = arg;

    assert(animate_params->input);
    assert(animate_params->ctx);
    assert(animate_params->wayland);

    animation_state_t state;
    anim_init_state(animate_params->ctx, &state);
    
    const struct timespec frame_delay = {0, state.frame_time_ns};
    
    atomic_store(&animate_params->ctx->_animation_running, true);
    bongocat_log_debug("Animation thread main loop started");
    
    while (atomic_load(&animate_params->ctx->_animation_running)) {
        anim_update_state(animate_params->ctx, animate_params->input, &state);
        draw_bar(animate_params->wayland);
        nanosleep(&frame_delay, NULL);
    }
    
    bongocat_log_debug("Animation thread main loop exited");

    BONGOCAT_FREE(animate_params);
    animate_params = NULL;

    return NULL;
}

// =============================================================================
// IMAGE LOADING MODULE
// =============================================================================

typedef struct {
    const unsigned char *data;
    size_t size;
    const char *name;
} embedded_image_t;

#define BONGOCAT_EMBEDDED_IMAGES_COUNT BONGOCAT_NUM_FRAMES
static embedded_image_t* init_bongocat_embedded_images(void) {
    static embedded_image_t bongocat_embedded_images[BONGOCAT_EMBEDDED_IMAGES_COUNT];

    bongocat_embedded_images[BONGOCAT_FRAME_BOTH_DOWN] = (embedded_image_t){bongo_cat_both_up_png, bongo_cat_both_up_png_size, "embedded bongo-cat-both-up.png"};
    bongocat_embedded_images[BONGOCAT_FRAME_LEFT_DOWN] = (embedded_image_t){bongo_cat_left_down_png, bongo_cat_left_down_png_size, "embedded bongo-cat-left-down.png"};
    bongocat_embedded_images[BONGOCAT_FRAME_RIGHT_DOWN] = (embedded_image_t){bongo_cat_right_down_png, bongo_cat_right_down_png_size, "embedded bongo-cat-right-down.png"};
    bongocat_embedded_images[BONGOCAT_FRAME_BOTH_UP] = (embedded_image_t){bongo_cat_both_down_png, bongo_cat_both_down_png_size, "embedded bongo-cat-both-down.png"};

    return bongocat_embedded_images;
}

#define DIGIMON_SPRITE_SHEET_EMBEDDED_IMAGES_COUNT TOTAL_ANIMATIONS
static embedded_image_t* init_digimon_embedded_images(void) {
    static embedded_image_t digimon_sprite_sheet_embedded_images[DIGIMON_SPRITE_SHEET_EMBEDDED_IMAGES_COUNT];

    // index 0 is reserved for bongocat, no sprite sheet exists
    digimon_sprite_sheet_embedded_images[BONGOCAT_ANIM_INDEX] = (embedded_image_t){NULL, 0, "bongocat.png"};

    /// @TODO: index more digimons here
    digimon_sprite_sheet_embedded_images[DM20_AGUMON_ANIM_INDEX] = (embedded_image_t){dm20_agumon_png, dm20_agumon_png_size, "embedded dm20/Agumon.png"};


    return digimon_sprite_sheet_embedded_images;
}

static void anim_cleanup_loaded_images(animation_frame_t *anim_imgs, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (anim_imgs[i].pixels) {
            stbi_image_free(anim_imgs[i].pixels);
            anim_imgs[i].pixels = NULL;
        }
        anim_imgs[i].width = 0;
        anim_imgs[i].height = 0;
    }
}

static void anim_free_pixels(animation_frame_t *anim_imgs, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (anim_imgs[i].pixels) {
            BONGOCAT_SAFE_FREE(anim_imgs[i].pixels);
        }
        anim_imgs[i].width = 0;
        anim_imgs[i].height = 0;
    }
}

static bongocat_error_t anim_load_embedded_images(animation_frame_t *anim_imgs, size_t anim_imgs_count, embedded_image_t *embedded_images, size_t embedded_images_count) {
    for (size_t i = 0; i < anim_imgs_count && i < embedded_images_count; i++) {
        const embedded_image_t *img = &embedded_images[i];
        
        bongocat_log_debug("Loading embedded image: %s", img->name);

        anim_imgs[i].channels = RGBA_CHANNELS;
        assert(img->size <= INT_MAX);
        anim_imgs[i].pixels = stbi_load_from_memory(img->data, (int)img->size,
                                                  &anim_imgs[i].width,
                                                  &anim_imgs[i].height,
                                                  NULL, anim_imgs[i].channels);
        if (!anim_imgs[i].pixels) {
            bongocat_log_error("Failed to load embedded image: %s", img->name);
            anim_cleanup_loaded_images(anim_imgs, i);
            return BONGOCAT_ERROR_FILE_IO;
        }
        
        bongocat_log_debug("Loaded %dx%d embedded image", anim_imgs[i].width, anim_imgs[i].height);
    }
    
    return BONGOCAT_SUCCESS;
}

static int anim_load_sprite_sheet(config_t *config, animation_frame_t *anim_imgs, size_t anim_imgs_count, embedded_image_t *sprite_sheet_image, int sprite_sheet_cols, int sprite_sheet_rows) {
    BONGOCAT_CHECK_NULL(config, -1);
    BONGOCAT_CHECK_NULL(anim_imgs, -1);
    BONGOCAT_CHECK_NULL(sprite_sheet_image, -1);

    if (sprite_sheet_cols < 0 || sprite_sheet_rows < 0) {
        return BONGOCAT_ERROR_INVALID_PARAM;
    }

    assert(sprite_sheet_image->size <= INT_MAX);

    int sprite_sheet_count = 0;
    int result = load_sprite_sheet_from_memory(anim_imgs, anim_imgs_count,
                                  sprite_sheet_image->data, (int)sprite_sheet_image->size,
                                  sprite_sheet_cols, sprite_sheet_rows,
                                  &sprite_sheet_count,
                                  config->padding_x, config->padding_y,
                                  config->invert_color ? COPY_PIXEL_OPTION_INVERT : COPY_PIXEL_OPTION_NORMAL);
    if (result != 0) {
        bongocat_log_error("Sprite Sheet load failed: %s", sprite_sheet_image->name);
        return -1;
    }
    if (sprite_sheet_count <= 0) {
        bongocat_log_error("Sprite Sheet is empty: %s", sprite_sheet_image->name);
        return 0;
    }

    // assume every frame is the same size, pick first frame
    bongocat_log_debug("Loaded %dx%d sprite sheet with %d frames", anim_imgs[0].width, anim_imgs[0].height, sprite_sheet_count);

    return sprite_sheet_count;
}

static bongocat_error_t init_digimon_anim(animation_context_t* ctx, int anim_index, embedded_image_t* sprite_sheet_image, int sprite_sheet_cols, int sprite_sheet_rows) {
    int sprite_sheet_count = anim_load_sprite_sheet(ctx->_current_config, ctx->anims[anim_index].frames, MAX_NUM_FRAMES, sprite_sheet_image, sprite_sheet_cols, sprite_sheet_rows);
    if (sprite_sheet_count < 0) {
        bongocat_log_error("Load Digimon Animation failed: %s, index: %d", sprite_sheet_image->name, anim_index);

        return BONGOCAT_ERROR_ANIMATION;
    }

    return BONGOCAT_SUCCESS;
}

// =============================================================================
// PUBLIC API IMPLEMENTATION
// =============================================================================

bongocat_error_t animation_init(animation_context_t* ctx, config_t *config) {
    BONGOCAT_CHECK_NULL(ctx, BONGOCAT_ERROR_INVALID_PARAM);
    BONGOCAT_CHECK_NULL(config, BONGOCAT_ERROR_INVALID_PARAM);

    bongocat_log_info("Initializing animation system");

    animation_update_config(ctx, config);
    ctx->anim_frame_index = config->idle_frame;

    ctx->anim_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    ctx->_animation_running = false;

    // empty animations
    for (size_t i = 0;i < TOTAL_ANIMATIONS; i++) {
        for (size_t j = 0; j < MAX_DIGIMON_FRAMES; j++) {
            ctx->anims[i].frames[j] = empty_sprite_sheet_frame;
        }
    }
    
    // Initialize embedded images data
    embedded_image_t* bongocat_embedded_images = init_bongocat_embedded_images();
    embedded_image_t* digimon_sprite_sheet_embedded_images = init_digimon_embedded_images();
    
    int result = anim_load_embedded_images(ctx->anims[BONGOCAT_ANIM_INDEX].frames, MAX_NUM_FRAMES,
                                           bongocat_embedded_images, BONGOCAT_EMBEDDED_IMAGES_COUNT);
    if (result != 0) {
        return result;
    }

    /// @TODO: load more digimon frames
    init_digimon_anim(ctx, DM20_AGUMON_ANIM_INDEX, &digimon_sprite_sheet_embedded_images[DM20_AGUMON_ANIM_INDEX], DM20_AGUMON_SPRITE_SHEET_COLS, DM20_AGUMON_SPRITE_SHEET_ROWS);
    
    bongocat_log_info("Animation system initialized successfully with embedded assets");
    return BONGOCAT_SUCCESS;
}

bongocat_error_t animation_start(animation_context_t* ctx, input_context_t *input, wayland_context_t *wayland) {
    BONGOCAT_CHECK_NULL(ctx, BONGOCAT_ERROR_INVALID_PARAM);
    BONGOCAT_CHECK_NULL(input, BONGOCAT_ERROR_INVALID_PARAM);
    BONGOCAT_CHECK_NULL(wayland, BONGOCAT_ERROR_INVALID_PARAM);

    anim_thread_main_params_t* anim_thread_main_arg = BONGOCAT_MALLOC(sizeof(anim_thread_main_params_t));
    if (!anim_thread_main_arg) {
        bongocat_log_error("Failed to allocate memory for animate_arg");
        return BONGOCAT_ERROR_MEMORY;
    }
    anim_thread_main_arg->input = input;
    anim_thread_main_arg->ctx = ctx;
    anim_thread_main_arg->wayland = wayland;

    bongocat_log_info("Starting animation thread");
    
    int result = pthread_create(&ctx->_anim_thread, NULL, anim_thread_main, anim_thread_main_arg);
    if (result != 0) {
        BONGOCAT_FREE(anim_thread_main_arg);
        bongocat_log_error("Failed to create animation thread: %s", strerror(result));
        return BONGOCAT_ERROR_THREAD;
    }
    // arg ownership has moved into thread
    anim_thread_main_arg = NULL;
    
    bongocat_log_debug("Animation thread started successfully");
    return BONGOCAT_SUCCESS;
}

void animation_cleanup(animation_context_t* ctx) {
    assert(ctx);

    if (atomic_load(&ctx->_animation_running)) {
        bongocat_log_debug("Stopping animation thread");
        atomic_store(&ctx->_animation_running, false);
        
        // Wait for thread to finish gracefully
        pthread_join(ctx->_anim_thread, NULL);
        bongocat_log_debug("Animation thread stopped");
    }

    // Cleanup loaded images
    // free bongocat images loaded from stbi
    anim_cleanup_loaded_images(ctx->anims[BONGOCAT_ANIM_INDEX].frames, MAX_NUM_FRAMES);
    // free allocated pixels (copied pixels)
    assert(BONGOCAT_ANIM_INDEX == 0);
    for (size_t i = 1;i < TOTAL_ANIMATIONS; i++) {
        anim_free_pixels(ctx->anims[i].frames, MAX_NUM_FRAMES);
    }
    
    // Cleanup mutex
    pthread_mutex_destroy(&ctx->anim_lock);
    
    bongocat_log_debug("Animation cleanup complete");
}

void animation_trigger(input_context_t *input) {
    assert(input);
    *input->any_key_pressed = 1;
}

void animation_update_config(animation_context_t *ctx, config_t *config) {
    assert(ctx);
    assert(config);

    ctx->_current_config = config;
    ctx->anim_index = config->animation_index;
}