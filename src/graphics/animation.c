#define _POSIX_C_SOURCE 200809L
#include "graphics/animation.h"

#include "graphics/paw_frame.h"
#include "utils/error.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
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

// Defined in assets.S as NUL-terminated SVG documents.
extern const char ASSET_BOTH_UP[], ASSET_LEFT_DOWN[], ASSET_RIGHT_DOWN[];
extern const char ASSET_BOTH_DOWN[], ASSET_SLEEPING[];

static const char *const ASSETS[NUM_FRAMES] = {
    [BONGOCAT_FRAME_BOTH_UP] = ASSET_BOTH_UP,
    [BONGOCAT_FRAME_LEFT_DOWN] = ASSET_LEFT_DOWN,
    [BONGOCAT_FRAME_RIGHT_DOWN] = ASSET_RIGHT_DOWN,
    [BONGOCAT_FRAME_BOTH_DOWN] = ASSET_BOTH_DOWN,
    [BONGOCAT_FRAME_SLEEPING] = ASSET_SLEEPING,
};
static NSVGimage *svgs[NUM_FRAMES];
static NSVGrasterizer *rasterizer;
static uint32_t random_state = 1;

// =============================================================================
// ARTWORK
// =============================================================================

void animation_cleanup(void) {
  for (int i = 0; i < NUM_FRAMES; i++) {
    nsvgDelete(svgs[i]);
    svgs[i] = NULL;
  }
  nsvgDeleteRasterizer(rasterizer);
  rasterizer = NULL;
}

bongocat_error_t animation_init(void) {
  for (int i = 0; i < NUM_FRAMES; i++) {
    // nsvgParse modifies its input.
    char *copy = strdup(ASSETS[i]);
    svgs[i] = copy ? nsvgParse(copy, "px", 96.0F) : NULL;
    free(copy);
    if (!svgs[i] || svgs[i]->width <= 0 || svgs[i]->height <= 0) {
      bongocat_log_error("Failed to parse embedded frame %d", i);
      animation_cleanup();
      return BONGOCAT_ERROR_MEMORY;
    }
  }
  rasterizer = nsvgCreateRasterizer();
  if (!rasterizer) {
    animation_cleanup();
    return BONGOCAT_ERROR_MEMORY;
  }
  struct timespec seed;
  clock_gettime(CLOCK_MONOTONIC, &seed);
  random_state = ((uint32_t)seed.tv_sec ^ (uint32_t)seed.tv_nsec) | 1U;
  return BONGOCAT_SUCCESS;
}

// =============================================================================
// FRAME CACHE
// =============================================================================

void frame_cache_free(frame_cache_t *cache) {
  for (int i = 0; i < NUM_FRAMES; i++) {
    free(cache->frames[i].data);
  }
  *cache = (frame_cache_t){0};
}

static uint8_t *rasterize(const NSVGimage *svg, int width, int height,
                          bool mirror_x, bool mirror_y) {
  size_t size = (size_t)width * (size_t)height * 4U;
  uint8_t *rgba = calloc(1, size);
  if (!rgba) {
    return NULL;
  }
  nsvgRasterize(rasterizer, (NSVGimage *)svg, 0, 0, (float)width / svg->width,
                rgba, width, height, width * 4);
  uint32_t *pixels = (uint32_t *)rgba;
  if (mirror_x) {
    for (int y = 0; y < height; y++) {
      uint32_t *row = pixels + ((size_t)y * width);
      for (int left = 0, right = width - 1; left < right; left++, right--) {
        uint32_t swap = row[left];
        row[left] = row[right];
        row[right] = swap;
      }
    }
  }
  if (mirror_y) {
    for (int top = 0, bottom = height - 1; top < bottom; top++, bottom--) {
      uint32_t *a = pixels + ((size_t)top * width);
      uint32_t *b = pixels + ((size_t)bottom * width);
      for (int x = 0; x < width; x++) {
        uint32_t swap = a[x];
        a[x] = b[x];
        b[x] = swap;
      }
    }
  }
  // RGBA to premultiplied BGRA, the byte order of WL_SHM_FORMAT_ARGB8888.
  for (size_t i = 0; i < size; i += 4) {
    uint8_t r = rgba[i];
    uint8_t g = rgba[i + 1];
    uint8_t b = rgba[i + 2];
    uint8_t a = rgba[i + 3];
    rgba[i] = (uint8_t)((b * a) / 255);
    rgba[i + 1] = (uint8_t)((g * a) / 255);
    rgba[i + 2] = (uint8_t)((r * a) / 255);
  }
  return rgba;
}

bool frame_cache_update(frame_cache_t *cache, int width, int height,
                        bool mirror_x, bool mirror_y) {
  if (cache->frames[0].data && cache->width == width &&
      cache->height == height && cache->mirror_x == mirror_x &&
      cache->mirror_y == mirror_y) {
    return true;
  }
  frame_cache_free(cache);
  if (!rasterizer || width <= 0 || height <= 0 ||
      (uint64_t)width * (uint64_t)height * 4 > UINT64_C(256) * 1024 * 1024) {
    return false;
  }
  for (int i = 0; i < NUM_FRAMES; i++) {
    cache->frames[i] = (cached_frame_t){
        .data = rasterize(svgs[i], width, height, mirror_x, mirror_y),
        .width = width,
        .height = height};
    if (!cache->frames[i].data) {
      frame_cache_free(cache);
      return false;
    }
  }
  cache->width = width;
  cache->height = height;
  cache->mirror_x = mirror_x;
  cache->mirror_y = mirror_y;
  bongocat_log_debug("Cached %d frames at %dx%d", NUM_FRAMES, width, height);
  return true;
}

void blit_frame(uint8_t *dest, int dest_width, int dest_height,
                const cached_frame_t *frame, int x, int y) {
  // Clip in 64-bit: offsets come from user configuration.
  int64_t x0 = x < 0 ? -(int64_t)x : 0;
  int64_t y0 = y < 0 ? -(int64_t)y : 0;
  int64_t x1 = (int64_t)dest_width - x;
  int64_t y1 = (int64_t)dest_height - y;
  x1 = x1 < frame->width ? x1 : frame->width;
  y1 = y1 < frame->height ? y1 : frame->height;
  for (int64_t row = y0; row < y1; row++) {
    const uint8_t *src = frame->data + ((size_t)row * frame->width * 4);
    uint8_t *out = dest + ((size_t)(row + y) * dest_width * 4);
    for (int64_t column = x0; column < x1; column++) {
      const uint8_t *s = src + (column * 4);
      uint8_t *d = out + ((column + x) * 4);
      uint8_t alpha = s[3];
      if (alpha == 255) {
        memcpy(d, s, 4);
      } else if (alpha) {
        // Premultiplied "over".
        for (int channel = 0; channel < 4; channel++) {
          d[channel] =
              (uint8_t)(s[channel] + ((d[channel] * (255 - alpha)) / 255));
        }
      }
    }
  }
}

// =============================================================================
// ANIMATION STATE
// =============================================================================

unsigned anim_random_paw(void) {
  // Cosmetic choice: xorshift, never the security-sensitive rand().
  random_state ^= random_state << 13;
  random_state ^= random_state >> 17;
  random_state ^= random_state << 5;
  return (random_state & 1U) ? PAW_LEFT : PAW_RIGHT;
}

static int64_t seconds_us(int seconds) {
  return (int64_t)seconds * 1000000;
}

void anim_reset(anim_state_t *state, const config_t *config, int64_t now) {
  *state = (anim_state_t){
      .next_test = now + seconds_us(config->test_animation_interval),
      .last_input = now,
      .frame = config->idle_frame,
  };
}

static void press(anim_state_t *state, unsigned paws, int64_t until) {
  if (paws & PAW_LEFT) {
    state->left_until = until;
  }
  if (paws & PAW_RIGHT) {
    state->right_until = until;
  }
}

static void earliest(int64_t *deadline, int64_t candidate, int64_t now) {
  if (candidate > now && (!*deadline || candidate < *deadline)) {
    *deadline = candidate;
  }
}

int64_t anim_step(anim_state_t *state, const config_t *config, unsigned paws,
                  const anim_step_t *step) {
  int64_t now = step->now;
  if (step->paused) {
    state->left_until = state->right_until = 0;
    state->frame = config->idle_frame;
    return 0;
  }
  // Input during scheduled sleep is discarded, not replayed afterwards.
  if (paws && !step->sleeping) {
    int64_t pressed = step->input_time > 0 && step->input_time <= now
                          ? step->input_time
                          : now;
    press(state, paws, pressed + ((int64_t)config->keypress_duration * 1000));
    state->last_input = pressed;
    state->next_test = pressed + seconds_us(config->test_animation_interval);
  }
  int64_t sleep_at =
      config->idle_sleep_timeout
          ? state->last_input + seconds_us(config->idle_sleep_timeout)
          : 0;
  if (step->sleeping || (sleep_at && now >= sleep_at)) {
    state->frame = BONGOCAT_FRAME_SLEEPING;
    return 0;  // Input or the schedule wakes the cat.
  }
  if (config->test_animation_interval && now >= state->next_test) {
    press(state, anim_random_paw(),
          now + ((int64_t)config->test_animation_duration * 1000));
    state->next_test = now + seconds_us(config->test_animation_interval);
  }
  state->frame = frame_from_paw_state(
      now < state->left_until, now < state->right_until, config->idle_frame);
  int64_t deadline = 0;
  earliest(&deadline, state->left_until, now);
  earliest(&deadline, state->right_until, now);
  if (config->test_animation_interval) {
    earliest(&deadline, state->next_test, now);
  }
  earliest(&deadline, sleep_at, now);
  return deadline;
}

bool sleep_schedule(const config_t *config, time_t now, time_t *next_change) {
  *next_change = 0;
  if (!config->enable_scheduled_sleep) {
    return false;
  }
  struct tm local;
  localtime_r(&now, &local);
  int minute = (local.tm_hour * 60) + local.tm_min;
  int begin = config->sleep_begin;
  int end = config->sleep_end;
  bool sleeping = begin < end ? minute >= begin && minute < end
                              : minute >= begin || minute < end;
  int target = sleeping ? end : begin;
  struct tm change = local;
  change.tm_hour = target / 60;
  change.tm_min = target % 60;
  change.tm_sec = 0;
  change.tm_isdst = -1;
  if (target <= minute) {
    change.tm_mday++;
  }
  *next_change = mktime(&change);
  if (*next_change <= now) {
    *next_change = now + 60;  // Unrepresentable or skipped local time
  }
  return sleeping;
}
