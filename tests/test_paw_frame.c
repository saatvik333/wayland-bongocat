// Unit tests for paw-state -> frame derivation
#define _POSIX_C_SOURCE 200809L

// Stub wayland-client types before including bongocat.h (matches test_memory.c)
struct wl_output;
struct zxdg_output_v1;
#define _WAYLAND_CLIENT_H
#define _XDG_OUTPUT_UNSTABLE_V1_CLIENT_PROTOCOL_H

#include "../include/core/bongocat.h"
#include "../include/graphics/paw_frame.h"

#include <stdio.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg)                                                 \
  do {                                                                         \
    if (cond) {                                                                \
      tests_passed++;                                                          \
    } else {                                                                   \
      tests_failed++;                                                          \
      fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, msg);        \
    }                                                                          \
  } while (0)

static void test_combinations(void) {
  printf("test_combinations...\n");
  int idle = BONGOCAT_FRAME_BOTH_UP;
  TEST_ASSERT(frame_from_paw_state(false, false, idle) == BONGOCAT_FRAME_BOTH_UP,
              "neither -> idle (both up)");
  TEST_ASSERT(frame_from_paw_state(true, false, idle) == BONGOCAT_FRAME_LEFT_DOWN,
              "left only -> left down");
  TEST_ASSERT(frame_from_paw_state(false, true, idle) == BONGOCAT_FRAME_RIGHT_DOWN,
              "right only -> right down");
  TEST_ASSERT(frame_from_paw_state(true, true, idle) == BONGOCAT_FRAME_BOTH_DOWN,
              "both -> both down");
}

static void test_custom_idle_frame(void) {
  printf("test_custom_idle_frame...\n");
  // A non-default idle_frame is honored when no paw is live.
  TEST_ASSERT(frame_from_paw_state(false, false, BONGOCAT_FRAME_SLEEPING) ==
                  BONGOCAT_FRAME_SLEEPING,
              "idle returns configured idle_frame");
}

int main(void) {
  printf("=== Paw Frame Tests ===\n");
  test_combinations();
  test_custom_idle_frame();
  printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
