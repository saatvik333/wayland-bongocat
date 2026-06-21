// Unit tests for the SPMC broadcast key ring
#define _POSIX_C_SOURCE 200809L

#include "../include/platform/key_ring.h"

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

// Basic push then drain returns codes in order.
static void test_push_drain_order(void) {
  printf("test_push_drain_order...\n");
  key_ring_t r;
  key_ring_init(&r);
  unsigned tail = 0;
  key_ring_push(&r, 30);  // A (left)
  key_ring_push(&r, 38);  // L (right)
  int out[KEY_RING_SIZE];
  unsigned n = key_ring_drain(&r, &tail, out, KEY_RING_SIZE);
  TEST_ASSERT(n == 2, "drained two codes");
  TEST_ASSERT(out[0] == 30, "first code is 30");
  TEST_ASSERT(out[1] == 38, "second code is 38");
  unsigned n2 = key_ring_drain(&r, &tail, out, KEY_RING_SIZE);
  TEST_ASSERT(n2 == 0, "second drain is empty");
}

// Two independent consumers each see the full stream (broadcast).
static void test_two_consumers(void) {
  printf("test_two_consumers...\n");
  key_ring_t r;
  key_ring_init(&r);
  unsigned tail_a = 0, tail_b = 0;
  for (int i = 0; i < 5; i++)
    key_ring_push(&r, 100 + i);
  int out[KEY_RING_SIZE];
  unsigned na = key_ring_drain(&r, &tail_a, out, KEY_RING_SIZE);
  unsigned nb = key_ring_drain(&r, &tail_b, out, KEY_RING_SIZE);
  TEST_ASSERT(na == 5, "consumer A sees all 5");
  TEST_ASSERT(nb == 5, "consumer B sees all 5 independently");
}

// Wrap-around past KEY_RING_SIZE works.
static void test_wraparound(void) {
  printf("test_wraparound...\n");
  key_ring_t r;
  key_ring_init(&r);
  unsigned tail = 0;
  int out[KEY_RING_SIZE];
  for (int i = 0; i < KEY_RING_SIZE + 10; i++) {
    key_ring_push(&r, i);
    unsigned n = key_ring_drain(&r, &tail, out, KEY_RING_SIZE);
    TEST_ASSERT(n == 1 && out[0] == i, "keep-up consumer reads each in turn");
  }
}

// A lapped consumer drops oldest, keeps newest KEY_RING_SIZE, never more.
static void test_overflow_drop_oldest(void) {
  printf("test_overflow_drop_oldest...\n");
  key_ring_t r;
  key_ring_init(&r);
  unsigned tail = 0;
  for (int i = 0; i < KEY_RING_SIZE + 5; i++)  // never drained -> lapped by 5
    key_ring_push(&r, i);
  int out[KEY_RING_SIZE];
  unsigned n = key_ring_drain(&r, &tail, out, KEY_RING_SIZE);
  TEST_ASSERT(n == KEY_RING_SIZE, "drain clamps to ring size");
  TEST_ASSERT(out[0] == 5, "oldest 5 dropped, first kept is 5");
  TEST_ASSERT(out[KEY_RING_SIZE - 1] == KEY_RING_SIZE + 4, "last kept is newest");
}

int main(void) {
  printf("=== Key Ring Tests ===\n");
  test_push_drain_order();
  test_two_consumers();
  test_wraparound();
  test_overflow_drop_oldest();
  printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
