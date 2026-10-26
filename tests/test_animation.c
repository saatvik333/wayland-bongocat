#define _POSIX_C_SOURCE 200809L
#include "graphics/animation.h"
#include "graphics/paw_frame.h"
#include "test_helpers.h"

#include <limits.h>
#include <stdlib.h>
#include <time.h>

#define MS INT64_C(1000)  // Microseconds

static config_t base_config(void) {
  return (config_t){.fps = 60,
                    .keypress_duration = 100,
                    .test_animation_duration = 200,
                    .enable_hand_mapping = 1};
}

static int64_t step(anim_state_t *state, const config_t *config, unsigned paws,
                    int64_t now) {
  anim_step_t input = {.now = now};
  return anim_step(state, config, paws, &input);
}

static void test_paws(void) {
  config_t config = base_config();
  anim_state_t state;
  anim_reset(&state, &config, 1000 * MS);
  TEST_ASSERT(step(&state, &config, 0, 1000 * MS) == 0);
  TEST_ASSERT(state.frame == BONGOCAT_FRAME_BOTH_UP);

  // A press holds its paw for keypress_duration from the press time.
  anim_step_t input = {.now = 1010 * MS, .input_time = 1005 * MS};
  TEST_ASSERT(anim_step(&state, &config, PAW_LEFT, &input) == 1105 * MS);
  TEST_ASSERT(state.frame == BONGOCAT_FRAME_LEFT_DOWN);
  TEST_ASSERT(step(&state, &config, PAW_RIGHT, 1050 * MS) == 1105 * MS);
  TEST_ASSERT(state.frame == BONGOCAT_FRAME_BOTH_DOWN);
  TEST_ASSERT(step(&state, &config, 0, 1105 * MS) == 1150 * MS);
  TEST_ASSERT(state.frame == BONGOCAT_FRAME_RIGHT_DOWN);
  TEST_ASSERT(step(&state, &config, 0, 1150 * MS) == 0);
  TEST_ASSERT(state.frame == BONGOCAT_FRAME_BOTH_UP);

  // Pausing shows the idle frame and drops held paws.
  config.idle_frame = BONGOCAT_FRAME_BOTH_DOWN;
  step(&state, &config, PAW_LEFT, 1200 * MS);
  input = (anim_step_t){.now = 1201 * MS, .paused = true};
  TEST_ASSERT(anim_step(&state, &config, 0, &input) == 0);
  TEST_ASSERT(state.frame == BONGOCAT_FRAME_BOTH_DOWN);
  TEST_ASSERT(step(&state, &config, 0, 1202 * MS) == 0);
  TEST_ASSERT(state.frame == BONGOCAT_FRAME_BOTH_DOWN);
}

static void test_sleep(void) {
  config_t config = base_config();
  config.idle_sleep_timeout = 10;
  anim_state_t state;
  anim_reset(&state, &config, 0);
  // Idle sleep starts idle_sleep_timeout after the last input.
  TEST_ASSERT(step(&state, &config, 0, 1000 * MS) == 10000 * MS);
  TEST_ASSERT(step(&state, &config, 0, 10000 * MS) == 0);
  TEST_ASSERT(state.frame == BONGOCAT_FRAME_SLEEPING);
  // Input wakes the cat and restarts the countdown.
  TEST_ASSERT(step(&state, &config, PAW_LEFT, 12000 * MS) == 12100 * MS);
  TEST_ASSERT(state.frame == BONGOCAT_FRAME_LEFT_DOWN);
  TEST_ASSERT(step(&state, &config, 0, 12100 * MS) == 22000 * MS);

  // Scheduled sleep discards input instead of replaying it afterwards.
  anim_step_t input = {.now = 13000 * MS, .sleeping = true};
  TEST_ASSERT(anim_step(&state, &config, PAW_RIGHT, &input) == 0);
  TEST_ASSERT(state.frame == BONGOCAT_FRAME_SLEEPING);
  TEST_ASSERT(step(&state, &config, 0, 13001 * MS) == 22000 * MS);
  TEST_ASSERT(state.frame == BONGOCAT_FRAME_BOTH_UP);
}

static void test_test_animation(void) {
  config_t config = base_config();
  config.test_animation_interval = 2;
  anim_state_t state;
  anim_reset(&state, &config, 0);
  TEST_ASSERT(step(&state, &config, 0, 1000 * MS) == 2000 * MS);
  TEST_ASSERT(step(&state, &config, 0, 2000 * MS) == 2200 * MS);
  TEST_ASSERT(state.frame == BONGOCAT_FRAME_LEFT_DOWN ||
              state.frame == BONGOCAT_FRAME_RIGHT_DOWN);
  TEST_ASSERT(step(&state, &config, 0, 2200 * MS) == 4000 * MS);
  // Real input postpones the next test press.
  step(&state, &config, PAW_LEFT, 3000 * MS);
  TEST_ASSERT(step(&state, &config, 0, 3100 * MS) == 5000 * MS);
}

static time_t local_time(int day, int hour, int minute) {
  struct tm tm = {.tm_year = 126,
                  .tm_mon = 0,
                  .tm_mday = day,
                  .tm_hour = hour,
                  .tm_min = minute,
                  .tm_isdst = -1};
  return mktime(&tm);
}

static void test_schedule(void) {
  config_t config = base_config();
  time_t next;
  TEST_ASSERT(!sleep_schedule(&config, local_time(1, 12, 0), &next) && !next);

  config.enable_scheduled_sleep = 1;
  config.sleep_begin = 22 * 60;
  config.sleep_end = 6 * 60;  // Overnight range
  TEST_ASSERT(!sleep_schedule(&config, local_time(1, 21, 59), &next));
  TEST_ASSERT(next == local_time(1, 22, 0));
  TEST_ASSERT(sleep_schedule(&config, local_time(1, 22, 0), &next));
  TEST_ASSERT(next == local_time(2, 6, 0));
  TEST_ASSERT(sleep_schedule(&config, local_time(2, 5, 59), &next));
  TEST_ASSERT(next == local_time(2, 6, 0));
  TEST_ASSERT(!sleep_schedule(&config, local_time(2, 6, 0), &next));
  TEST_ASSERT(next == local_time(2, 22, 0));

  config.sleep_begin = 9 * 60;
  config.sleep_end = 17 * 60;  // Daytime range
  TEST_ASSERT(sleep_schedule(&config, local_time(1, 12, 30), &next));
  TEST_ASSERT(next == local_time(1, 17, 0));
  TEST_ASSERT(!sleep_schedule(&config, local_time(1, 17, 30), &next));
  TEST_ASSERT(next == local_time(2, 9, 0));
}

static void test_rendering(void) {
  TEST_ASSERT(animation_init() == BONGOCAT_SUCCESS);
  frame_cache_t cache = {0};
  TEST_ASSERT(frame_cache_update(&cache, 50, 28, false, false));
  uint8_t *first = cache.frames[0].data;
  TEST_ASSERT(first && cache.frames[NUM_FRAMES - 1].data);
  TEST_ASSERT(frame_cache_update(&cache, 50, 28, false, false));
  TEST_ASSERT(cache.frames[0].data == first);  // Cached
  TEST_ASSERT(frame_cache_update(&cache, 50, 28, true, false));
  TEST_ASSERT(cache.mirror_x);
  TEST_ASSERT(!frame_cache_update(&cache, 0, 28, false, false));
  TEST_ASSERT(!cache.frames[0].data);
  frame_cache_free(&cache);
  animation_cleanup();

  // Blits clip at every edge, including extreme offsets.
  uint8_t pixel[] = {10, 20, 30, 255};
  cached_frame_t frame = {.data = pixel, .width = 1, .height = 1};
  uint8_t dest[2 * 2 * 4] = {0};
  blit_frame(dest, 2, 2, &frame, INT_MIN, INT_MAX);
  blit_frame(dest, 2, 2, &frame, 2, 0);
  blit_frame(dest, 2, 2, &frame, 0, -1);
  for (size_t i = 0; i < sizeof(dest); i++) {
    TEST_ASSERT(dest[i] == 0);
  }
  blit_frame(dest, 2, 2, &frame, 1, 1);
  TEST_ASSERT(dest[12] == 10 && dest[15] == 255);
  // Translucent pixels composite with premultiplied "over".
  uint8_t half[] = {50, 50, 50, 128};
  frame.data = half;
  uint8_t under[4] = {200, 200, 200, 255};
  blit_frame(under, 1, 1, &frame, 0, 0);
  TEST_ASSERT(under[0] == 50 + (200 * 127 / 255) && under[3] == 255);
}

int main(void) {
  setenv("TZ", "UTC", 1);
  tzset();
  test_paws();
  test_sleep();
  test_test_animation();
  test_schedule();
  test_rendering();
  return 0;
}
