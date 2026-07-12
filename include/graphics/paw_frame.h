#ifndef BONGOCAT_PAW_FRAME_H
#define BONGOCAT_PAW_FRAME_H

// Shared pending-paw bits. Input child ORs bits; animation thread atomically
// consumes them. Sleep handling is caller responsibility.

#include "core/bongocat.h"

#include <stdbool.h>

#define PAW_LEFT  1u
#define PAW_RIGHT 2u
#define PAW_BOTH  (PAW_LEFT | PAW_RIGHT)

static inline unsigned paw_for_keycode(int keycode) {
  static const int left_keys[] = {
      1,  2,  3,  4,  5,  6,  7,  15, 16, 17, 18, 19, 20, 29,  30,
      31, 32, 33, 34, 41, 42, 44, 45, 46, 47, 48, 56, 58, 125,
  };
  for (size_t i = 0; i < sizeof(left_keys) / sizeof(left_keys[0]); i++) {
    if (keycode == left_keys[i]) {
      return PAW_LEFT;
    }
  }
  return PAW_RIGHT;
}

static inline unsigned paw_apply_mirror(unsigned paws, bool mirror) {
  if (!mirror) {
    return paws;
  }
  return ((paws & PAW_LEFT) ? PAW_RIGHT : 0u) |
         ((paws & PAW_RIGHT) ? PAW_LEFT : 0u);
}

static inline int frame_from_paw_state(bool left_live, bool right_live,
                                       int idle_frame) {
  if (left_live && right_live) {
    return BONGOCAT_FRAME_BOTH_DOWN;
  }
  if (left_live) {
    return BONGOCAT_FRAME_LEFT_DOWN;
  }
  if (right_live) {
    return BONGOCAT_FRAME_RIGHT_DOWN;
  }
  return idle_frame;
}

#endif  // BONGOCAT_PAW_FRAME_H
