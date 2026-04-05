// Unit tests for memory pool and allocation

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

// Stub out wayland-client.h types before including headers
struct wl_output;
struct zxdg_output_v1;
#define _WAYLAND_CLIENT_H
#define _XDG_OUTPUT_UNSTABLE_V1_CLIENT_PROTOCOL_H

#include "../include/core/bongocat.h"
#include "../include/utils/error.h"
#include "../include/utils/memory.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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

// ---------------------------------------------------------------------------
// Test: pool creation and basic allocation
// ---------------------------------------------------------------------------
static void test_pool_create(void) {
  printf("test_pool_create...\n");
  memory_pool_t *pool = memory_pool_create(4096, 16);
  TEST_ASSERT(pool != NULL, "pool created successfully");
  TEST_ASSERT(pool->size == 4096, "pool size is 4096");
  TEST_ASSERT(pool->used == 0, "pool starts empty");
  TEST_ASSERT(pool->alignment == 16, "pool alignment is 16");
  memory_pool_destroy(pool);
}

// ---------------------------------------------------------------------------
// Test: pool alignment
// ---------------------------------------------------------------------------
static void test_pool_alignment(void) {
  printf("test_pool_alignment...\n");
  memory_pool_t *pool = memory_pool_create(4096, 16);
  TEST_ASSERT(pool != NULL, "pool created");

  void *a = memory_pool_alloc(pool, 1);
  void *b = memory_pool_alloc(pool, 1);
  TEST_ASSERT(a != NULL, "first alloc succeeds");
  TEST_ASSERT(b != NULL, "second alloc succeeds");
  TEST_ASSERT(((uintptr_t)a % 16) == 0, "first alloc is 16-aligned");
  TEST_ASSERT(((uintptr_t)b % 16) == 0, "second alloc is 16-aligned");
  TEST_ASSERT((char *)b - (char *)a >= 16,
              "allocations are at least alignment apart");

  memory_pool_destroy(pool);
}

// ---------------------------------------------------------------------------
// Test: pool exhaustion
// ---------------------------------------------------------------------------
static void test_pool_exhaustion(void) {
  printf("test_pool_exhaustion...\n");
  memory_pool_t *pool = memory_pool_create(64, 8);
  TEST_ASSERT(pool != NULL, "small pool created");

  // Allocate until exhaustion
  void *a = memory_pool_alloc(pool, 32);
  void *b = memory_pool_alloc(pool, 32);
  TEST_ASSERT(a != NULL, "first 32-byte alloc");
  TEST_ASSERT(b != NULL, "second 32-byte alloc");

  void *c = memory_pool_alloc(pool, 32);
  TEST_ASSERT(c == NULL, "third alloc returns NULL (exhausted)");

  memory_pool_destroy(pool);
}

// ---------------------------------------------------------------------------
// Test: pool reset
// ---------------------------------------------------------------------------
static void test_pool_reset(void) {
  printf("test_pool_reset...\n");
  memory_pool_t *pool = memory_pool_create(256, 8);
  TEST_ASSERT(pool != NULL, "pool created");

  void *a = memory_pool_alloc(pool, 128);
  TEST_ASSERT(a != NULL, "alloc before reset");
  TEST_ASSERT(pool->used > 0, "pool has used space");

  memory_pool_reset(pool);
  TEST_ASSERT(pool->used == 0, "pool reset to 0");

  void *b = memory_pool_alloc(pool, 128);
  TEST_ASSERT(b != NULL, "alloc after reset succeeds");

  memory_pool_destroy(pool);
}

// ---------------------------------------------------------------------------
// Test: calloc overflow detection
// ---------------------------------------------------------------------------
static void test_calloc_overflow(void) {
  printf("test_calloc_overflow...\n");
  // SIZE_MAX / 2 * 3 would overflow
  void *p = bongocat_calloc(SIZE_MAX / 2, 3);
  TEST_ASSERT(p == NULL, "calloc overflow returns NULL");

  // Zero count should return NULL
  void *q = bongocat_calloc(0, 100);
  TEST_ASSERT(q == NULL, "calloc with count=0 returns NULL");
}

// ---------------------------------------------------------------------------
// Test: malloc and free
// ---------------------------------------------------------------------------
static void test_malloc_free(void) {
  printf("test_malloc_free...\n");
  void *p = bongocat_malloc(256);
  TEST_ASSERT(p != NULL, "malloc(256) succeeds");
  bongocat_free(p);

  // Zero-size malloc
  void *z = bongocat_malloc(0);
  // Behavior is implementation-defined; just ensure no crash
  if (z)
    bongocat_free(z);
  tests_passed++;  // If we got here, no crash
}

int main(void) {
  bongocat_error_init(0);
  printf("=== Memory Pool Tests ===\n");

  test_pool_create();
  test_pool_alignment();
  test_pool_exhaustion();
  test_pool_reset();
  test_calloc_overflow();
  test_malloc_free();

  printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
