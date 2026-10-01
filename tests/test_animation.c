#define _POSIX_C_SOURCE 200809L
#include "graphics/animation.h"
#include "graphics/paw_frame.h"
#include "platform/input.h"
#include "platform/wayland.h"
#include "test_helpers.h"

#include <limits.h>
#include <time.h>
atomic_uint *pending_paws;
static unsigned redraws;
void wayland_request_current_redraw(void) {
  redraws++;
}
int64_t input_timestamp(void) {
  return 0;
}
static void delay(int ms) {
  struct timespec ts = {.tv_nsec = ms * 1000000L};
  nanosleep(&ts, NULL);
}
int main(void) {
  config_t config = {.fps = 1,
                     .keypress_duration = 100,
                     .idle_frame = 0,
                     .enable_hand_mapping = 1};
  TEST_ASSERT(animation_init(&config) == BONGOCAT_SUCCESS);
  void *first = animation_overlay_create(&config);
  void *second = animation_overlay_create(&config);
  TEST_ASSERT(first && second);
  animation_overlay_activate(first, &config);
  TEST_ASSERT(animation_tick(0) == -1);
  unsigned before = redraws;
  int deadline = animation_tick(PAW_LEFT);
  TEST_ASSERT(deadline > 0 && deadline <= 1000);
  TEST_ASSERT(anim_index == BONGOCAT_FRAME_LEFT_DOWN && redraws == before);
  config.fps = 120;
  animation_tick(PAW_LEFT);
  TEST_ASSERT(redraws > before);
  animation_overlay_activate(second, &config);
  animation_tick(PAW_RIGHT);
  TEST_ASSERT(anim_index == BONGOCAT_FRAME_RIGHT_DOWN);
  animation_overlay_activate(first, &config);
  animation_tick(0);
  TEST_ASSERT(anim_index == BONGOCAT_FRAME_LEFT_DOWN);
  animation_set_paused(true);
  animation_tick(PAW_RIGHT);
  TEST_ASSERT(anim_index == 0);
  animation_set_paused(false);
  animation_tick(0);
  TEST_ASSERT(anim_index == 0);
  delay(110);
  animation_overlay_activate(second, &config);
  animation_tick(0);
  TEST_ASSERT(anim_index == 0);
  animation_overlay_cache(4, 4);
  uint8_t *cache = anim_cached_frames[0].data;
  TEST_ASSERT(cache);
  animation_overlay_cache(4, 4);
  TEST_ASSERT(cache == anim_cached_frames[0].data);
  uint8_t source[] = {0, 0, 255, 255};
  uint8_t dest[16] = {0};
  blit_cached_frame(dest, 2, 2, source, 1, 1, INT_MIN, INT_MAX);
  for (size_t i = 0; i < sizeof(dest); i++)
    TEST_ASSERT(dest[i] == 0);
  blit_cached_frame(dest, 2, 2, source, 1, 1, 1, 1);
  TEST_ASSERT(dest[14] == 255 && dest[15] == 255);
  animation_overlay_destroy(first);
  animation_overlay_destroy(second);
  animation_cleanup();
  return 0;
}
