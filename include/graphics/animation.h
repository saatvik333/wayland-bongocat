#ifndef ANIMATION_H
#define ANIMATION_H

#include "config/config.h"
#include "core/bongocat.h"
#include "utils/error.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef struct {
  uint8_t *data;  // Premultiplied BGRA, width * height pixels
  int width;
  int height;
} cached_frame_t;

// Every frame rasterized at one size and orientation.
typedef struct {
  cached_frame_t frames[NUM_FRAMES];
  int width;
  int height;
  bool mirror_x;
  bool mirror_y;
} frame_cache_t;

// Paw and sleep timers of one overlay. Times are monotonic microseconds.
typedef struct {
  int64_t left_until;
  int64_t right_until;
  int64_t next_test;
  int64_t last_input;
  int frame;
} anim_state_t;

// Inputs shared by every overlay for one animation step.
typedef struct {
  int64_t now;
  int64_t input_time;  // When the newest paws were pressed; 0 if unknown
  bool paused;         // Show the idle frame and ignore input
  bool sleeping;       // Scheduled sleep is active
} anim_step_t;

// Parses the embedded artwork shared by every overlay.
[[nodiscard]] bongocat_error_t animation_init(void);
void animation_cleanup(void);

void anim_reset(anim_state_t *state, const config_t *config, int64_t now);
// Applies paws (PAW_* bits, already mapped for this overlay) and selects
// state->frame. Returns the next monotonic time the frame may change, or 0.
int64_t anim_step(anim_state_t *state, const config_t *config, unsigned paws,
                  const anim_step_t *step);
// A random paw, for enable_hand_mapping=0.
unsigned anim_random_paw(void);

// Whether scheduled sleep is active at wall time now; *next_change receives
// the next wall time this can change.
bool sleep_schedule(const config_t *config, time_t now, time_t *next_change);

// Rasterizes the frames unless already cached with these parameters.
[[nodiscard]] bool frame_cache_update(frame_cache_t *cache, int width,
                                      int height, bool mirror_x, bool mirror_y);
void frame_cache_free(frame_cache_t *cache);

// Composites a frame over dest with its top-left corner at (x, y), clipped.
void blit_frame(uint8_t *dest, int dest_width, int dest_height,
                const cached_frame_t *frame, int x, int y);

#endif  // ANIMATION_H
