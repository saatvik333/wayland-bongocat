#include "graphics/paw_frame.h"
#include "test_helpers.h"

#include <linux/input-event-codes.h>

int main(void) {
  int idle = BONGOCAT_FRAME_BOTH_UP;
  TEST_ASSERT(frame_from_paw_state(false, false, idle) == idle);
  TEST_ASSERT(frame_from_paw_state(true, false, idle) ==
              BONGOCAT_FRAME_LEFT_DOWN);
  TEST_ASSERT(frame_from_paw_state(false, true, idle) ==
              BONGOCAT_FRAME_RIGHT_DOWN);
  TEST_ASSERT(frame_from_paw_state(true, true, idle) ==
              BONGOCAT_FRAME_BOTH_DOWN);
  TEST_ASSERT(frame_from_paw_state(false, false, BONGOCAT_FRAME_SLEEPING) ==
              BONGOCAT_FRAME_SLEEPING);

  // Keyboard halves: the left side of a QWERTY board plays the left paw.
  TEST_ASSERT(paw_for_keycode(KEY_A) == PAW_LEFT);
  TEST_ASSERT(paw_for_keycode(KEY_LEFTSHIFT) == PAW_LEFT);
  TEST_ASSERT(paw_for_keycode(KEY_L) == PAW_RIGHT);
  TEST_ASSERT(paw_for_keycode(KEY_ENTER) == PAW_RIGHT);

  TEST_ASSERT(paw_apply_mirror(PAW_LEFT, false) == PAW_LEFT);
  TEST_ASSERT(paw_apply_mirror(PAW_LEFT, true) == PAW_RIGHT);
  TEST_ASSERT(paw_apply_mirror(PAW_RIGHT, true) == PAW_LEFT);
  TEST_ASSERT(paw_apply_mirror(PAW_BOTH, true) == PAW_BOTH);
  return 0;
}
