#define _POSIX_C_SOURCE 199309L
#define STB_IMAGE_IMPLEMENTATION
#include "graphics/animation.h"

#include "graphics/embedded_assets.h"
#include "platform/input.h"
#include "platform/wayland.h"
#include "utils/memory.h"

#include <time.h>

// =============================================================================
// GLOBAL STATE AND CONFIGURATION
// =============================================================================

// Animation frame data
unsigned char *anim_imgs[NUM_FRAMES];
int anim_width[NUM_FRAMES], anim_height[NUM_FRAMES];
int anim_index = 0;
pthread_mutex_t anim_lock = PTHREAD_MUTEX_INITIALIZER;

// Animation system state
static config_t *current_config;
static pthread_t anim_thread;
static volatile bool animation_running = false;

// =============================================================================
// DRAWING OPERATIONS MODULE
// =============================================================================

static bool drawing_is_pixel_in_bounds(int x, int y, int width, int height) {
  return (x >= 0 && y >= 0 && x < width && y < height);
}

static void drawing_copy_pixel(uint8_t *dest, const unsigned char *src,
                               int dest_idx, int src_idx) {
  dest[dest_idx + 0] = src[src_idx + 2];  // B
  dest[dest_idx + 1] = src[src_idx + 1];  // G
  dest[dest_idx + 2] = src[src_idx + 0];  // R
  dest[dest_idx + 3] = src[src_idx + 3];  // A
}

static void drawing_copy_pixel_rgba(uint8_t *dest, int dest_idx, uint8_t r,
                                    uint8_t g, uint8_t b, uint8_t a) {
  dest[dest_idx + 0] = b;  // B
  dest[dest_idx + 1] = g;  // G
  dest[dest_idx + 2] = r;  // R
  dest[dest_idx + 3] = a;  // A
}

// Alpha blend source pixel onto destination - enables smooth anti-aliased edges
static void drawing_blend_pixel(uint8_t *dest, int dest_idx, uint8_t src_r,
                                uint8_t src_g, uint8_t src_b, uint8_t src_a) {
  // Skip fully transparent pixels
  if (src_a == 0) {
    return;
  }

  // Fully opaque - direct copy (fast path)
  if (src_a == 255) {
    dest[dest_idx + 0] = src_b;
    dest[dest_idx + 1] = src_g;
    dest[dest_idx + 2] = src_r;
    dest[dest_idx + 3] = 255;
    return;
  }

  // Alpha blend: out = src * alpha + dest * (1 - alpha)
  float alpha = src_a / 255.0f;
  float inv_alpha = 1.0f - alpha;

  uint8_t dest_b = dest[dest_idx + 0];
  uint8_t dest_g = dest[dest_idx + 1];
  uint8_t dest_r = dest[dest_idx + 2];

  dest[dest_idx + 0] = (uint8_t)(src_b * alpha + dest_b * inv_alpha + 0.5f);
  dest[dest_idx + 1] = (uint8_t)(src_g * alpha + dest_g * inv_alpha + 0.5f);
  dest[dest_idx + 2] = (uint8_t)(src_r * alpha + dest_r * inv_alpha + 0.5f);
  dest[dest_idx + 3] = 255;
}

// Box filter for high-quality downscaling - averages all source pixels that
// map to a destination pixel. Produces much smoother results than bilinear
// when shrinking images significantly.
static void drawing_get_box_filtered_pixel(const unsigned char *src, int src_w,
                                           int src_h, int dest_x, int dest_y,
                                           int target_w, int target_h,
                                           int mirror_x, int mirror_y,
                                           uint8_t *r, uint8_t *g, uint8_t *b,
                                           uint8_t *a) {
  // Calculate the source region that maps to this destination pixel
  float src_x_start = ((float)dest_x * src_w) / target_w;
  float src_x_end = ((float)(dest_x + 1) * src_w) / target_w;
  float src_y_start = ((float)dest_y * src_h) / target_h;
  float src_y_end = ((float)(dest_y + 1) * src_h) / target_h;

  // Clamp to image bounds
  int x0 = (int)src_x_start;
  int x1 = (int)src_x_end;
  int y0 = (int)src_y_start;
  int y1 = (int)src_y_end;

  if (x0 < 0)
    x0 = 0;
  if (y0 < 0)
    y0 = 0;
  if (x1 >= src_w)
    x1 = src_w - 1;
  if (y1 >= src_h)
    y1 = src_h - 1;
  if (x1 < x0)
    x1 = x0;
  if (y1 < y0)
    y1 = y0;

  // Accumulate all pixels in the source region
  float sum_r = 0, sum_g = 0, sum_b = 0, sum_a = 0;
  int count = 0;

  for (int sy = y0; sy <= y1; sy++) {
    for (int sx = x0; sx <= x1; sx++) {
      int mx = mirror_x ? (src_w - 1 - sx) : sx;
      int my = mirror_y ? (src_h - 1 - sy) : sy;
      int idx = (my * src_w + mx) * 4;

      sum_r += src[idx + 0];
      sum_g += src[idx + 1];
      sum_b += src[idx + 2];
      sum_a += src[idx + 3];
      count++;
    }
  }

  // Average the accumulated values
  if (count > 0) {
    *r = (uint8_t)(sum_r / count + 0.5f);
    *g = (uint8_t)(sum_g / count + 0.5f);
    *b = (uint8_t)(sum_b / count + 0.5f);
    *a = (uint8_t)(sum_a / count + 0.5f);
  } else {
    *r = *g = *b = *a = 0;
  }
}

// Bilinear interpolation for smooth scaling
static void drawing_get_interpolated_pixel(const unsigned char *src, int src_w,
                                           int src_h, float fx, float fy,
                                           uint8_t *r, uint8_t *g, uint8_t *b,
                                           uint8_t *a) {
  // Clamp coordinates to image bounds
  if (fx < 0)
    fx = 0;
  if (fy < 0)
    fy = 0;
  if (fx >= src_w - 1)
    fx = src_w - 1;
  if (fy >= src_h - 1)
    fy = src_h - 1;

  int x1 = (int)fx;
  int y1 = (int)fy;
  int x2 = x1 + 1;
  int y2 = y1 + 1;

  // Clamp to bounds
  if (x2 >= src_w)
    x2 = src_w - 1;
  if (y2 >= src_h)
    y2 = src_h - 1;

  float dx = fx - x1;
  float dy = fy - y1;

  // Get the four surrounding pixels
  int idx_tl = (y1 * src_w + x1) * 4;  // top-left
  int idx_tr = (y1 * src_w + x2) * 4;  // top-right
  int idx_bl = (y2 * src_w + x1) * 4;  // bottom-left
  int idx_br = (y2 * src_w + x2) * 4;  // bottom-right

  // Interpolate each channel
  for (int c = 0; c < 4; c++) {
    float top = src[idx_tl + c] * (1.0f - dx) + src[idx_tr + c] * dx;
    float bottom = src[idx_bl + c] * (1.0f - dx) + src[idx_br + c] * dx;
    float result = top * (1.0f - dy) + bottom * dy;

    switch (c) {
    case 0:
      *r = (uint8_t)(result + 0.5f);
      break;  // R
    case 1:
      *g = (uint8_t)(result + 0.5f);
      break;  // G
    case 2:
      *b = (uint8_t)(result + 0.5f);
      break;  // B
    case 3:
      *a = (uint8_t)(result + 0.5f);
      break;  // A
    }
  }
}

void blit_image_scaled(uint8_t *dest, int dest_w, int dest_h,
                       unsigned char *src, int src_w, int src_h, int offset_x,
                       int offset_y, int target_w, int target_h) {
  // OPTIMIZATION: Hoist invariants outside loops
  const bool use_aa = current_config && current_config->enable_antialiasing;
  const bool mirror_x = current_config && current_config->mirror_x;
  const bool mirror_y = current_config && current_config->mirror_y;
  const bool is_downscaling = (target_w < src_w) || (target_h < src_h);

  // Pre-calculate scale factors (avoid division in loop)
  const float scale_x = (float)src_w / target_w;
  const float scale_y = (float)src_h / target_h;

  for (int y = 0; y < target_h; y++) {
    const int dy = y + offset_y;

    // Skip entire row if out of bounds
    if (dy < 0 || dy >= dest_h)
      continue;

    // Pre-calculate row offset
    const int row_offset = dy * dest_w * 4;

    for (int x = 0; x < target_w; x++) {
      const int dx = x + offset_x;

      // Skip if out of horizontal bounds
      if (dx < 0 || dx >= dest_w)
        continue;

      const int dest_idx = row_offset + dx * 4;

      if (use_aa) {
        uint8_t r, g, b, a;

        if (is_downscaling) {
          // Box filter: average all source pixels that map to this dest pixel
          drawing_get_box_filtered_pixel(src, src_w, src_h, x, y, target_w,
                                         target_h, mirror_x, mirror_y, &r, &g,
                                         &b, &a);
        } else {
          // Bilinear interpolation for upscaling
          float fx = x * scale_x;
          float fy = y * scale_y;

          // Apply mirroring
          if (mirror_x)
            fx = (src_w - 1) - fx;
          if (mirror_y)
            fy = (src_h - 1) - fy;

          drawing_get_interpolated_pixel(src, src_w, src_h, fx, fy, &r, &g, &b,
                                         &a);
        }

        // Alpha blend onto destination (enables smooth anti-aliased edges)
        drawing_blend_pixel(dest, dest_idx, r, g, b, a);
      } else {
        // Use nearest-neighbor scaling (original behavior)
        int sx = (x * src_w) / target_w;
        int sy = (y * src_h) / target_h;

        // Apply mirroring
        if (mirror_x)
          sx = (src_w - 1) - sx;
        if (mirror_y)
          sy = (src_h - 1) - sy;

        const int src_idx = (sy * src_w + sx) * 4;

        // Only draw non-transparent pixels
        if (src[src_idx + 3] > 128) {
          drawing_copy_pixel(dest, src, dest_idx, src_idx);
        }
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

      int idx = (j * width + i) * 4;
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
  long hold_until;
  int test_counter;
  int test_interval_frames;
  long frame_time_ns;
  long last_key_pressed_timestamp;
} animation_state_t;

static long anim_get_current_time_us(void) {
  struct timeval now;
  gettimeofday(&now, NULL);
  return now.tv_sec * 1000000 + now.tv_usec;
}

static bool anim_is_sleep_time(const config_t *config) {
  time_t raw_time;
  struct tm time_info;
  time(&raw_time);
  localtime_r(&raw_time, &time_info);

  const int now_minutes = time_info.tm_hour * 60 + time_info.tm_min;
  const int begin = config->sleep_begin.hour * 60 + config->sleep_begin.min;
  const int end = config->sleep_end.hour * 60 + config->sleep_end.min;

  // Normal range (e.g., 10:00–22:00): begin < end && (now_minutes >= begin &&
  // now_minutes < end) Overnight range (e.g., 22:00–06:00): begin > end &&
  // (now_minutes >= begin || now_minutes < end)
  return (begin == end) ||
         (begin < end ? (now_minutes >= begin && now_minutes < end)
                      : (now_minutes >= begin || now_minutes < end));
}

// Get frame based on keyboard position (left=1, right=2)
// Uses Linux input keycodes from <linux/input-event-codes.h>
static int get_frame_for_keycode(int keycode) {
  // Left-hand keys on QWERTY keyboard
  // clang-format off
  static const int left_keys[] = {
    // Number row left half (1-6)
    2, 3, 4, 5, 6, 7,           // KEY_1 to KEY_6
    // QWERTY row left half
    16, 17, 18, 19, 20,         // KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T
    // Home row left half
    30, 31, 32, 33, 34,         // KEY_A, KEY_S, KEY_D, KEY_F, KEY_G
    // Bottom row left half
    44, 45, 46, 47, 48,         // KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B
    // Modifiers and special keys (left side)
    1,                          // KEY_ESC
    15,                         // KEY_TAB
    58,                         // KEY_CAPSLOCK
    42,                         // KEY_LEFTSHIFT
    29,                         // KEY_LEFTCTRL
    56,                         // KEY_LEFTALT
    41,                         // KEY_GRAVE (backtick)
    125,                        // KEY_LEFTMETA (super)
  };
  // clang-format on

  for (size_t i = 0; i < sizeof(left_keys) / sizeof(left_keys[0]); i++) {
    if (keycode == left_keys[i]) {
      return 1;  // Left hand
    }
  }
  return 2;  // Right hand (default for all other keys)
}

static int anim_get_active_frame(void) {
  if (current_config && current_config->enable_hand_mapping) {
    int keycode = atomic_load(last_key_code);
    int frame = get_frame_for_keycode(keycode);
    // Flip hands when cat is mirrored horizontally
    if (current_config->mirror_x) {
      frame = (frame == 1) ? 2 : 1;
    }
    return frame;
  }
  return (rand() % 2) + 1;  // Random: frame 1 or 2
}

static void anim_trigger_frame_change(int new_frame, long duration_us,
                                      long current_time_us,
                                      animation_state_t *state) {
  if (current_config->enable_debug) {
    bongocat_log_debug("Animation frame change: %d (duration: %ld us)",
                       new_frame, duration_us);
  }

  anim_index = new_frame;
  state->hold_until = current_time_us + duration_us;
}

static void anim_handle_test_animation(animation_state_t *state,
                                       long current_time_us) {
  if (current_config->test_animation_interval <= 0) {
    return;
  }

  state->test_counter++;
  if (state->test_counter > state->test_interval_frames) {
    int new_frame = anim_get_active_frame();
    long duration_us = current_config->test_animation_duration * 1000;

    bongocat_log_debug("Test animation trigger");
    anim_trigger_frame_change(new_frame, duration_us, current_time_us, state);
    state->test_counter = 0;
  }
}

static void anim_handle_key_press(animation_state_t *state,
                                  long current_time_us) {
  if (!atomic_load(any_key_pressed)) {
    return;
  }

  if (!current_config->enable_scheduled_sleep ||
      !anim_is_sleep_time(current_config)) {
    int new_frame = anim_get_active_frame();
    long duration_us = current_config->keypress_duration * 1000;

    bongocat_log_debug("Key press detected - switching to frame %d", new_frame);
    anim_trigger_frame_change(new_frame, duration_us, current_time_us, state);

    atomic_store(any_key_pressed, 0);
    state->test_counter = 0;  // Reset test counter
    state->last_key_pressed_timestamp = current_time_us;
  }
}

static void anim_handle_idle_return(animation_state_t *state,
                                    long current_time_us) {
  int show_sleep_frame = 0;
  // Sleep Mode
  if (current_config->enable_scheduled_sleep) {
    if (anim_is_sleep_time(current_config)) {
      show_sleep_frame = 1;
    }
  }
  // Idle Sleep
  if (current_config->idle_sleep_timeout_sec > 0 &&
      state->last_key_pressed_timestamp > 0) {
    if (anim_get_current_time_us() - state->last_key_pressed_timestamp >=
        current_config->idle_sleep_timeout_sec * 1000000L) {
      show_sleep_frame = 1;
    }
  }

  if (show_sleep_frame) {
    if (anim_index != BONGOCAT_FRAME_BOTH_DOWN) {
      bongocat_log_debug("Returning to sleep frame");
      anim_index = BONGOCAT_FRAME_BOTH_DOWN;
    }
    return;
  }

  if (current_time_us <= state->hold_until) {
    return;
  }

  if (anim_index != current_config->idle_frame) {
    bongocat_log_debug("Returning to idle frame %d",
                       current_config->idle_frame);
    anim_index = current_config->idle_frame;
  }
}

static void anim_update_state(animation_state_t *state) {
  long current_time_us = anim_get_current_time_us();

  pthread_mutex_lock(&anim_lock);

  anim_handle_test_animation(state, current_time_us);
  anim_handle_key_press(state, current_time_us);
  anim_handle_idle_return(state, current_time_us);

  pthread_mutex_unlock(&anim_lock);
}

// =============================================================================
// ANIMATION THREAD MANAGEMENT MODULE
// =============================================================================

static void anim_init_state(animation_state_t *state) {
  state->hold_until = 0;
  state->test_counter = 0;
  state->test_interval_frames =
      current_config->test_animation_interval * current_config->fps;
  state->frame_time_ns = 1000000000L / current_config->fps;
  state->last_key_pressed_timestamp = anim_get_current_time_us();
}

static void *anim_thread_main(void *arg __attribute__((unused))) {
  animation_state_t state;
  anim_init_state(&state);

  struct timespec frame_delay = {0, state.frame_time_ns};

  animation_running = true;
  bongocat_log_debug("Animation thread main loop started");

  // Track last drawn state to skip redundant redraws
  int last_drawn_frame = -1;
  bool force_redraw = true;  // Force first draw

  while (animation_running) {
    int prev_frame = anim_index;
    anim_update_state(&state);

    // Check if frame actually changed
    bool frame_changed = (anim_index != last_drawn_frame);
    bool state_changed = (anim_index != prev_frame);

    // Only redraw if something changed
    if (frame_changed || force_redraw) {
      draw_bar();
      last_drawn_frame = anim_index;
      force_redraw = false;
    }

    // Reduce sleep when actively animating, increase when idle
    if (state_changed) {
      // Active animation - use configured frame rate
      nanosleep(&frame_delay, NULL);
    } else {
      // Idle - can sleep a bit longer (reduce polling frequency)
      // Clamp to prevent nanoseconds overflow (max 999999999)
      long idle_ns = state.frame_time_ns * 2;
      if (idle_ns > 999999999L)
        idle_ns = 999999999L;
      struct timespec idle_delay = {0, idle_ns};
      nanosleep(&idle_delay, NULL);
    }
  }

  bongocat_log_debug("Animation thread main loop exited");
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

static embedded_image_t embedded_images[NUM_FRAMES];

static void init_embedded_images(void) {
  embedded_images[BONGOCAT_FRAME_BOTH_UP] =
      (embedded_image_t){bongo_cat_both_up_png, bongo_cat_both_up_png_size,
                         "embedded bongo-cat-both-up.png"};
  embedded_images[BONGOCAT_FRAME_LEFT_DOWN] =
      (embedded_image_t){bongo_cat_left_down_png, bongo_cat_left_down_png_size,
                         "embedded bongo-cat-left-down.png"};
  embedded_images[BONGOCAT_FRAME_RIGHT_DOWN] = (embedded_image_t){
      bongo_cat_right_down_png, bongo_cat_right_down_png_size,
      "embedded bongo-cat-right-down.png"};
  embedded_images[BONGOCAT_FRAME_BOTH_DOWN] =
      (embedded_image_t){bongo_cat_both_down_png, bongo_cat_both_down_png_size,
                         "embedded bongo-cat-both-down.png"};
}

static void anim_cleanup_loaded_images(int count) {
  for (int i = 0; i < count; i++) {
    if (anim_imgs[i]) {
      stbi_image_free(anim_imgs[i]);
      anim_imgs[i] = NULL;
    }
  }
}

static bongocat_error_t anim_load_embedded_images(void) {
  for (int i = 0; i < NUM_FRAMES; i++) {
    const embedded_image_t *img = &embedded_images[i];

    bongocat_log_debug("Loading embedded image: %s", img->name);

    anim_imgs[i] = stbi_load_from_memory(img->data, img->size, &anim_width[i],
                                         &anim_height[i], NULL, 4);
    if (!anim_imgs[i]) {
      bongocat_log_error("Failed to load embedded image: %s", img->name);
      anim_cleanup_loaded_images(i);
      return BONGOCAT_ERROR_FILE_IO;
    }

    bongocat_log_debug("Loaded %dx%d embedded image", anim_width[i],
                       anim_height[i]);
  }

  return BONGOCAT_SUCCESS;
}

// =============================================================================
// PUBLIC API IMPLEMENTATION
// =============================================================================

bongocat_error_t animation_init(config_t *config) {
  BONGOCAT_CHECK_NULL(config, BONGOCAT_ERROR_INVALID_PARAM);

  current_config = config;
  bongocat_log_info("Initializing animation system");

  // Initialize embedded images data
  init_embedded_images();

  bongocat_error_t result = anim_load_embedded_images();
  if (result != BONGOCAT_SUCCESS) {
    return result;
  }

  bongocat_log_info(
      "Animation system initialized successfully with embedded assets");
  return BONGOCAT_SUCCESS;
}

bongocat_error_t animation_start(void) {
  bongocat_log_info("Starting animation thread");

  int result = pthread_create(&anim_thread, NULL, anim_thread_main, NULL);
  if (result != 0) {
    bongocat_log_error("Failed to create animation thread: %s",
                       strerror(result));
    return BONGOCAT_ERROR_THREAD;
  }

  bongocat_log_debug("Animation thread started successfully");
  return BONGOCAT_SUCCESS;
}

void animation_cleanup(void) {
  if (animation_running) {
    bongocat_log_debug("Stopping animation thread");
    animation_running = false;

    // Wait for thread to finish gracefully
    pthread_join(anim_thread, NULL);
    bongocat_log_debug("Animation thread stopped");
  }

  // Cleanup loaded images
  anim_cleanup_loaded_images(NUM_FRAMES);

  // Cleanup mutex
  pthread_mutex_destroy(&anim_lock);

  bongocat_log_debug("Animation cleanup complete");
}

void animation_trigger(void) {
  atomic_store(any_key_pressed, 1);
}