#ifndef BONGOCAT_PAW_FRAME_H
#define BONGOCAT_PAW_FRAME_H

// Pure mapping from per-paw liveness to a cat frame index.
// Sleep handling is the caller's responsibility (checked before this).

#include "core/bongocat.h"

#include <stdbool.h>

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
