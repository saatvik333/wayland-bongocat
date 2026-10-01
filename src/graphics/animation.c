#define _POSIX_C_SOURCE 199309L
#include "config/config.h"
#include "core/bongocat.h"
#include "utils/error.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "graphics/animation.h"
#include "graphics/embedded_assets.h"
#include "graphics/paw_frame.h"
#include "platform/input.h"
#include "platform/wayland.h"
#ifdef __GNUC__
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#  pragma GCC diagnostic ignored "-Wdouble-promotion"
#  pragma GCC diagnostic ignored "-Wmissing-prototypes"
#  pragma GCC diagnostic ignored "-Wstrict-prototypes"
#  pragma GCC diagnostic ignored "-Wold-style-definition"
#endif
#include <nanosvg.h>
#include <nanosvgrast.h>
#ifdef __GNUC__
#  pragma GCC diagnostic pop
#endif
#include <unistd.h>

// =============================================================================
// GLOBAL STATE AND CONFIGURATION
// =============================================================================

int anim_index = 0;
cached_frame_t anim_cached_frames[NUM_FRAMES] = {0};

// SVG parsed data and rasterizer
static NSVGimage *anim_svgs[NUM_FRAMES];
static NSVGrasterizer *anim_rasterizer;

// Animation system state
static config_t *current_config;
static bool paused;
static unsigned reset_generation;
static uint32_t random_state = 1;
// Cosmetic paw selection uses a local PRNG, never security-sensitive rand().
static unsigned random_paw(void) {
  random_state ^= random_state << 13;
  random_state ^= random_state >> 17;
  random_state ^= random_state << 5;
  return random_state & 1U;
}
static bool animation_initialized = false;

// =============================================================================
// ANIMATION STATE MANAGEMENT MODULE
// =============================================================================

typedef struct {
  int64_t left_hold_until;  // paw down while now_us < left_hold_until
  int64_t right_hold_until;
  int64_t next_test_timestamp;
  int64_t last_key_pressed_timestamp;
} animation_state_t;

static int64_t anim_get_current_time_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((int64_t)ts.tv_sec * 1000000L) + (ts.tv_nsec / 1000L);
}

static bool anim_is_sleep_time(const config_t *config) {
  time_t raw_time;
  struct tm time_info;
  time(&raw_time);
  localtime_r(&raw_time, &time_info);

  const int now_minutes = (time_info.tm_hour * 60) + time_info.tm_min;
  const int begin = (config->sleep_begin.hour * 60) + config->sleep_begin.min;
  const int end = (config->sleep_end.hour * 60) + config->sleep_end.min;

  // Normal range (e.g., 10:00–22:00): begin < end && (now_minutes >= begin &&
  // now_minutes < end) Overnight range (e.g., 22:00–06:00): begin > end &&
  // (now_minutes >= begin || now_minutes < end)
  return ((begin == end) ||
          (begin < end ? (now_minutes >= begin && now_minutes < end)
                       : (now_minutes >= begin || now_minutes < end))) != 0;
}

// Extend the given paw's deadline to now + keypress_duration.
static void anim_press_paw(animation_state_t *state, int paw_frame,
                           int64_t current_time_us, int64_t duration_us) {
  if (paw_frame == BONGOCAT_FRAME_LEFT_DOWN) {
    state->left_hold_until = current_time_us + duration_us;
  } else {
    state->right_hold_until = current_time_us + duration_us;
  }
}

static void anim_handle_test_animation(animation_state_t *state,
                                       int64_t current_time_us) {
  if (current_config->test_animation_interval <= 0) {
    return;
  }

  if (current_time_us >= state->next_test_timestamp) {
    bongocat_log_debug("Test animation trigger");
    int paw =
        random_paw() ? BONGOCAT_FRAME_LEFT_DOWN : BONGOCAT_FRAME_RIGHT_DOWN;
    anim_press_paw(state, paw, current_time_us,
                   current_config->test_animation_duration * 1000L);
    state->next_test_timestamp =
        current_time_us + (current_config->test_animation_interval * 1000000L);
  }
}

static void anim_take_pending_paws(animation_state_t *state,
                                   int64_t current_time_us) {
  unsigned paws = pending_paws ? atomic_exchange_explicit(pending_paws, 0U,
                                                          memory_order_acquire)
                               : 0U;
  if (paws == 0U) {
    return;
  }
  // Exchange first: events received while sleeping are discarded, not replayed.
  if (current_config->enable_scheduled_sleep &&
      anim_is_sleep_time(current_config)) {
    return;
  }
  if (!current_config->enable_hand_mapping) {
    paws = random_paw() ? PAW_LEFT : PAW_RIGHT;
  } else {
    paws = paw_apply_mirror(paws, current_config->mirror_x != 0);
  }
  int64_t stamp = input_timestamp();
  if (stamp > 0 && stamp <= current_time_us) {
    current_time_us = stamp;
  }
  int64_t duration_us = current_config->keypress_duration * 1000L;
  if (paws & PAW_LEFT) {
    anim_press_paw(state, BONGOCAT_FRAME_LEFT_DOWN, current_time_us,
                   duration_us);
  }
  if (paws & PAW_RIGHT) {
    anim_press_paw(state, BONGOCAT_FRAME_RIGHT_DOWN, current_time_us,
                   duration_us);
  }
  state->last_key_pressed_timestamp = current_time_us;
  state->next_test_timestamp =
      current_time_us + (current_config->test_animation_interval * 1000000L);
}

// Derive anim_index from sleep state, then per-paw deadlines.
static void anim_select_frame(animation_state_t *state,
                              int64_t current_time_us) {
  int show_sleep_frame = 0;
  if (current_config->enable_scheduled_sleep &&
      anim_is_sleep_time(current_config)) {
    show_sleep_frame = 1;
  }
  if (current_config->idle_sleep_timeout_sec > 0 &&
      state->last_key_pressed_timestamp > 0 &&
      anim_get_current_time_us() - state->last_key_pressed_timestamp >=
          current_config->idle_sleep_timeout_sec * 1000000L) {
    show_sleep_frame = 1;
  }

  if (show_sleep_frame) {
    if (anim_index != BONGOCAT_FRAME_SLEEPING) {
      bongocat_log_debug("Returning to sleep frame");
      anim_index = BONGOCAT_FRAME_SLEEPING;
    }
    return;
  }

  bool left_live = current_time_us < state->left_hold_until;
  bool right_live = current_time_us < state->right_hold_until;
  int new_frame =
      frame_from_paw_state(left_live, right_live, current_config->idle_frame);
  if (new_frame != anim_index && current_config->enable_debug) {
    bongocat_log_debug("Frame -> %d (left=%d right=%d)", new_frame,
                       (int)left_live, (int)right_live);
  }
  anim_index = new_frame;
}

static void anim_update_state(animation_state_t *state) {
  int64_t current_time_us = anim_get_current_time_us();

  anim_handle_test_animation(state, current_time_us);
  anim_take_pending_paws(state, current_time_us);
  anim_select_frame(state, current_time_us);
}

// =============================================================================
// ANIMATION THREAD MANAGEMENT MODULE
// =============================================================================

static void anim_init_state(animation_state_t *state) {
  state->left_hold_until = 0;
  state->right_hold_until = 0;
  state->next_test_timestamp =
      anim_get_current_time_us() +
      (current_config->test_animation_interval * 1000000L);
  state->last_key_pressed_timestamp = anim_get_current_time_us();
}

typedef struct animation_overlay {
  animation_state_t state;
  cached_frame_t frames[NUM_FRAMES];
  int index;
  int width, height, mirror_x, mirror_y, aa;
  int last_drawn;
  int64_t next_draw_us;
  int fps;
  unsigned generation;
} animation_overlay_t;
static animation_overlay_t *active_animation;

void *animation_overlay_create(config_t *config) {
  current_config = config;
  animation_overlay_t *ctx = calloc(1, sizeof(*ctx));
  if (ctx) {
    anim_init_state(&ctx->state);
    ctx->last_drawn = -1;
  }
  return ctx;
}

void animation_overlay_activate(void *opaque, config_t *config) {
  animation_overlay_t *ctx = opaque;
  current_config = config;
  active_animation = ctx;
  anim_index = ctx->index;
  memcpy(anim_cached_frames, ctx->frames, sizeof(ctx->frames));
}

void animation_overlay_cache(int width, int height) {
  animation_overlay_t *ctx = active_animation;
  if (!ctx) {
    return;
  }
  if (ctx->width == width && ctx->height == height &&
      ctx->mirror_x == current_config->mirror_x &&
      ctx->mirror_y == current_config->mirror_y &&
      ctx->aa == current_config->enable_antialiasing) {
    return;
  }
  animation_cache_frames(width, height, current_config->mirror_x,
                         current_config->mirror_y,
                         current_config->enable_antialiasing);
  memcpy(ctx->frames, anim_cached_frames, sizeof(ctx->frames));
  ctx->width = width;
  ctx->height = height;
  ctx->mirror_x = current_config->mirror_x;
  ctx->mirror_y = current_config->mirror_y;
  ctx->aa = current_config->enable_antialiasing;
  ctx->last_drawn = -1;
}

void animation_overlay_destroy(void *opaque) {
  animation_overlay_t *ctx = opaque;
  if (!ctx) {
    return;
  }
  for (int i = 0; i < NUM_FRAMES; i++) {
    free(ctx->frames[i].data);
  }
  if (ctx == active_animation) {
    memset(anim_cached_frames, 0, sizeof(anim_cached_frames));
    active_animation = NULL;
  }
  free(ctx);
}

int animation_tick(unsigned paws) {
  animation_overlay_t *ctx = active_animation;
  if (!ctx) {
    return -1;
  }
  if (ctx->generation != reset_generation) {
    anim_init_state(&ctx->state);
    ctx->generation = reset_generation;
    ctx->last_drawn = -1;
    ctx->next_draw_us = 0;
  }
  int64_t now = anim_get_current_time_us();
  if (ctx->fps != current_config->fps) {
    ctx->fps = current_config->fps;
    ctx->next_draw_us = now;
  }
  atomic_uint local;
  atomic_init(&local, (int)paused ? 0 : paws);
  atomic_uint *saved = pending_paws;
  pending_paws = &local;
  if (paused) {
    ctx->state.left_hold_until = ctx->state.right_hold_until = 0;
    anim_index = current_config->idle_frame;
  } else {
    anim_update_state(&ctx->state);
  }
  pending_paws = saved;
  ctx->index = anim_index;
  int64_t deadline = 0;
  if (ctx->last_drawn != anim_index) {
    if (now >= ctx->next_draw_us || paused) {
      wayland_request_current_redraw();
      ctx->last_drawn = anim_index;
      ctx->next_draw_us = now + (1000000L / current_config->fps);
    } else {
      { deadline = ctx->next_draw_us; }
    }
  }
  int64_t candidates[] = {
      ctx->state.left_hold_until, ctx->state.right_hold_until,
      current_config->test_animation_interval > 0
          ? ctx->state.next_test_timestamp
          : 0,
      current_config->idle_sleep_timeout_sec > 0
          ? ctx->state.last_key_pressed_timestamp +
                (current_config->idle_sleep_timeout_sec * 1000000L)
          : 0};
  if (!paused) {
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
      if (candidates[i] > now && (!deadline || candidates[i] < deadline)) {
        deadline = candidates[i];
      }
    }
  }
  if (!paused && current_config->enable_scheduled_sleep) {
    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    int64_t next_minute =
        now + ((60 - (wall.tv_sec % 60)) * 1000000L) - (wall.tv_nsec / 1000);
    if (!deadline || next_minute < deadline) {
      deadline = next_minute;
    }
  }
  return deadline ? (int)((deadline - now + 999) / 1000) : -1;
}

void animation_set_paused(bool value) {
  paused = value;
  reset_generation++;
}

// =============================================================================
// SVG LOADING MODULE
// =============================================================================

typedef struct {
  const unsigned char *data;
  size_t size;
  const char *name;
} embedded_svg_t;

static embedded_svg_t embedded_svgs[NUM_FRAMES];

static void init_embedded_svgs(void) {
  embedded_svgs[BONGOCAT_FRAME_BOTH_UP] = (embedded_svg_t){
      bongo_both_up_svg, bongo_both_up_svg_size, "bongo-both-up.svg"};
  embedded_svgs[BONGOCAT_FRAME_LEFT_DOWN] = (embedded_svg_t){
      bongo_left_down_svg, bongo_left_down_svg_size, "bongo-left-down.svg"};
  embedded_svgs[BONGOCAT_FRAME_RIGHT_DOWN] = (embedded_svg_t){
      bongo_right_down_svg, bongo_right_down_svg_size, "bongo-right-down.svg"};
  embedded_svgs[BONGOCAT_FRAME_BOTH_DOWN] = (embedded_svg_t){
      bongo_both_down_svg, bongo_both_down_svg_size, "bongo-both-down.svg"};
  embedded_svgs[BONGOCAT_FRAME_SLEEPING] = (embedded_svg_t){
      bongo_sleeping_svg, bongo_sleeping_svg_size, "bongo-sleeping.svg"};
}

static void anim_cleanup_svgs(void) {
  for (int i = 0; i < NUM_FRAMES; i++) {
    if (anim_svgs[i]) {
      nsvgDelete(anim_svgs[i]);
      anim_svgs[i] = NULL;
    }
  }
  if (anim_rasterizer) {
    nsvgDeleteRasterizer(anim_rasterizer);
    anim_rasterizer = NULL;
  }
}

static bongocat_error_t anim_parse_embedded_svgs(void) {
  for (int i = 0; i < NUM_FRAMES; i++) {
    const embedded_svg_t *svg = &embedded_svgs[i];

    bongocat_log_debug("Parsing embedded SVG: %s", svg->name);

    // nsvgParse modifies the string in-place, so make a mutable copy
    char *svg_copy = malloc(svg->size + 1);
    if (!svg_copy) {
      bongocat_log_error("Failed to allocate SVG copy for: %s", svg->name);
      anim_cleanup_svgs();
      return BONGOCAT_ERROR_MEMORY;
    }
    memcpy(svg_copy, svg->data, svg->size);
    svg_copy[svg->size] = '\0';

    anim_svgs[i] = nsvgParse(svg_copy, "px", 96.0F);
    free(svg_copy);

    if (!anim_svgs[i]) {
      bongocat_log_error("Failed to parse embedded SVG: %s", svg->name);
      anim_cleanup_svgs();
      return BONGOCAT_ERROR_FILE_IO;
    }

    bongocat_log_debug("Parsed SVG %s: %.0fx%.0f", svg->name,
                       (double)anim_svgs[i]->width,
                       (double)anim_svgs[i]->height);
  }

  anim_rasterizer = nsvgCreateRasterizer();
  if (!anim_rasterizer) {
    bongocat_log_error("Failed to create SVG rasterizer");
    anim_cleanup_svgs();
    return BONGOCAT_ERROR_MEMORY;
  }

  return BONGOCAT_SUCCESS;
}

// =============================================================================
// FRAME CACHE MODULE
// =============================================================================

void animation_invalidate_cache(void) {
  for (int i = 0; i < NUM_FRAMES; i++) {
    free(anim_cached_frames[i].data);
    anim_cached_frames[i].data = NULL;
    anim_cached_frames[i].width = 0;
    anim_cached_frames[i].height = 0;
  }
}

void animation_cache_frames(int target_w, int target_h, int mirror_x,
                            int mirror_y, [[maybe_unused]] int enable_aa) {
  animation_invalidate_cache();

  if (!anim_rasterizer || target_w <= 0 || target_h <= 0 ||
      target_w > INT32_MAX / 4 ||
      (uint64_t)target_w * target_h * 4 > UINT64_C(256) * 1024 * 1024) {
    return;
  }

  for (int i = 0; i < NUM_FRAMES; i++) {
    if (!anim_svgs[i]) {
      continue;
    }

    float svg_w = anim_svgs[i]->width;
    float svg_h = anim_svgs[i]->height;
    if (svg_w <= 0 || svg_h <= 0) {
      continue;
    }

    // Rasterize SVG at exact target dimensions
    float scale = (float)target_w / svg_w;
    size_t buf_size = (size_t)target_w * (size_t)target_h * 4U;
    uint8_t *rgba_buf = calloc(1, buf_size);
    if (!rgba_buf) {
      bongocat_log_error("Failed to allocate raster buffer for frame %d", i);
      continue;
    }

    nsvgRasterize(anim_rasterizer, anim_svgs[i], 0, 0, scale, rgba_buf,
                  target_w, target_h, target_w * 4);

    // Apply horizontal mirror
    if (mirror_x) {
      for (int y = 0; y < target_h; y++) {
        for (int left = 0, right = target_w - 1; left < right;
             left++, right--) {
          int li = ((y * target_w) + left) * 4;
          int ri = ((y * target_w) + right) * 4;
          uint8_t tmp[4];
          memcpy(tmp, &rgba_buf[li], 4);
          memcpy(&rgba_buf[li], &rgba_buf[ri], 4);
          memcpy(&rgba_buf[ri], tmp, 4);
        }
      }
    }

    // Apply vertical mirror
    if (mirror_y) {
      size_t row_bytes = (size_t)target_w * 4U;
      uint8_t *tmp_row = malloc(row_bytes);
      if (tmp_row) {
        for (int top = 0, bot = target_h - 1; top < bot; top++, bot--) {
          uint8_t *t = &rgba_buf[(size_t)top * row_bytes];
          uint8_t *b = &rgba_buf[(size_t)bot * row_bytes];
          memcpy(tmp_row, t, row_bytes);
          memcpy(t, b, row_bytes);
          memcpy(b, tmp_row, row_bytes);
        }
        free(tmp_row);
      }
    }

    // Convert RGBA -> premultiplied BGRA for Wayland (ARGB8888 is
    // premultiplied)
    for (size_t px = 0; px < buf_size; px += 4) {
      uint8_t r = rgba_buf[px + 0];
      uint8_t g = rgba_buf[px + 1];
      uint8_t b = rgba_buf[px + 2];
      uint8_t a = rgba_buf[px + 3];
      rgba_buf[px + 0] = (uint8_t)((b * a) / 255);
      rgba_buf[px + 1] = (uint8_t)((g * a) / 255);
      rgba_buf[px + 2] = (uint8_t)((r * a) / 255);
      rgba_buf[px + 3] = a;
    }

    anim_cached_frames[i].data = rgba_buf;
    anim_cached_frames[i].width = target_w;
    anim_cached_frames[i].height = target_h;
  }

  bongocat_log_debug("Cached %d animation frames at %dx%d", NUM_FRAMES,
                     target_w, target_h);
}

void blit_cached_frame(uint8_t *dest, int dest_w, int dest_h,
                       const uint8_t *src, int src_w, int src_h, int offset_x,
                       int offset_y) {
  for (int y = 0; y < src_h; y++) {
    int64_t dy = (int64_t)y + offset_y;
    if (dy < 0 || dy >= dest_h) {
      continue;
    }
    for (int x = 0; x < src_w; x++) {
      int64_t dx = (int64_t)x + offset_x;
      if (dx < 0 || dx >= dest_w) {
        continue;
      }
      size_t si = (((size_t)y * src_w) + x) * 4;
      size_t di = (((size_t)dy * dest_w) + (size_t)dx) * 4;
      uint8_t sa = src[si + 3];
      if (sa == 0) {
        continue;
      }
      if (sa == 255) {
        memcpy(&dest[di], &src[si], 4);
      } else {
        // Premultiplied alpha "over" compositing
        uint8_t inv_a = 255 - sa;
        dest[di + 0] = src[si + 0] + (uint8_t)((dest[di + 0] * inv_a) / 255);
        dest[di + 1] = src[si + 1] + (uint8_t)((dest[di + 1] * inv_a) / 255);
        dest[di + 2] = src[si + 2] + (uint8_t)((dest[di + 2] * inv_a) / 255);
        dest[di + 3] = sa + (uint8_t)((dest[di + 3] * inv_a) / 255);
      }
    }
  }
}

// =============================================================================
// PUBLIC API IMPLEMENTATION
// =============================================================================

bongocat_error_t animation_init(config_t *config) {
  BONGOCAT_CHECK_NULL(config, BONGOCAT_ERROR_INVALID_PARAM);

  current_config = config;
  bongocat_log_info("Initializing animation system");

  // Parse embedded SVG assets
  init_embedded_svgs();

  bongocat_error_t result = anim_parse_embedded_svgs();
  if (result != BONGOCAT_SUCCESS) {
    return result;
  }

  animation_initialized = true;

  // Seed the random number generator so frame selection varies between runs
  struct timespec seed;
  clock_gettime(CLOCK_MONOTONIC, &seed);
  random_state = (uint32_t)seed.tv_sec ^ (uint32_t)seed.tv_nsec;
  if (random_state == 0) {
    random_state = 1;
  }

  bongocat_log_info(
      "Animation system initialized successfully with embedded SVG assets");
  return BONGOCAT_SUCCESS;
}

bongocat_error_t animation_start(void) {
  return BONGOCAT_SUCCESS;
}

void animation_cleanup(void) {
  // Cleanup cached frames
  animation_invalidate_cache();

  // Cleanup SVG resources
  if (animation_initialized) {
    anim_cleanup_svgs();
    animation_initialized = false;
  }

  bongocat_log_debug("Animation cleanup complete");
}
