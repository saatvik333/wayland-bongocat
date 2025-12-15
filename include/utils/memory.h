#ifndef MEMORY_H
#define MEMORY_H

#include "utils/error.h"

#include <stdint.h>
#include <stdlib.h>

// =============================================================================
// C23 MODERN FEATURES
// =============================================================================

// Nodiscard attribute for functions that return values that must be used
#if __STDC_VERSION__ >= 202311L
#  define BONGOCAT_NODISCARD [[nodiscard]]
#else
#  define BONGOCAT_NODISCARD __attribute__((warn_unused_result))
#endif

// Null pointer (C23 nullptr or fallback)
#if __STDC_VERSION__ >= 202311L
#  define BONGOCAT_NULLPTR nullptr
#else
#  define BONGOCAT_NULLPTR NULL
#endif

// =============================================================================
// RAII CLEANUP MACROS
// =============================================================================

// Cleanup function for auto-freeing malloc'd memory
static inline void bongocat_auto_free_impl(void *ptr) {
  void **p = (void **)ptr;
  if (*p) {
    free(*p);
    *p = BONGOCAT_NULLPTR;
  }
}

// Auto-free heap allocations when variable goes out of scope
#define BONGOCAT_AUTO_FREE __attribute__((cleanup(bongocat_auto_free_impl)))

// Safe free macro that nullifies pointer after freeing
#define BONGOCAT_SAFE_FREE(ptr)     \
  do {                              \
    if ((ptr)) {                    \
      bongocat_free((void *)(ptr)); \
      (ptr) = BONGOCAT_NULLPTR;     \
    }                               \
  } while (0)

// Forward declaration for pool cleanup
struct memory_pool;
static inline void bongocat_auto_pool_impl(struct memory_pool **pool);

// Auto-destroy memory pool when variable goes out of scope
#define BONGOCAT_AUTO_POOL __attribute__((cleanup(bongocat_auto_pool_impl)))

// =============================================================================
// MEMORY POOL
// =============================================================================

typedef struct memory_pool {
  void *data;
  size_t size;
  size_t used;
  size_t alignment;
} memory_pool_t;

// =============================================================================
// MEMORY ALLOCATION FUNCTIONS
// =============================================================================

// All allocation functions are nodiscard - caller must handle the result
BONGOCAT_NODISCARD void *bongocat_malloc(size_t size);
BONGOCAT_NODISCARD void *bongocat_calloc(size_t count, size_t size);
BONGOCAT_NODISCARD void *bongocat_realloc(void *ptr, size_t size);
void bongocat_free(void *ptr);

// =============================================================================
// MEMORY POOL FUNCTIONS
// =============================================================================

BONGOCAT_NODISCARD memory_pool_t *memory_pool_create(size_t size,
                                                     size_t alignment);
BONGOCAT_NODISCARD void *memory_pool_alloc(memory_pool_t *pool, size_t size);
void memory_pool_reset(memory_pool_t *pool);
void memory_pool_destroy(memory_pool_t *pool);

// Cleanup implementation for auto pool (must be after memory_pool_t definition)
static inline void bongocat_auto_pool_impl(memory_pool_t **pool) {
  if (*pool) {
    memory_pool_destroy(*pool);
    *pool = BONGOCAT_NULLPTR;
  }
}

// =============================================================================
// MEMORY STATISTICS
// =============================================================================

typedef struct {
  size_t total_allocated;
  size_t current_allocated;
  size_t peak_allocated;
  size_t allocation_count;
  size_t free_count;
} memory_stats_t;

void memory_get_stats(memory_stats_t *stats);
void memory_print_stats(void);

// =============================================================================
// DEBUG BUILD FEATURES
// =============================================================================

#ifdef DEBUG
#  define BONGOCAT_MALLOC(size) bongocat_malloc_debug(size, __FILE__, __LINE__)
#  define BONGOCAT_FREE(ptr)    bongocat_free_debug(ptr, __FILE__, __LINE__)
BONGOCAT_NODISCARD void *bongocat_malloc_debug(size_t size, const char *file,
                                               int line);
void bongocat_free_debug(void *ptr, const char *file, int line);
void memory_leak_check(void);
#else
#  define BONGOCAT_MALLOC(size) bongocat_malloc(size)
#  define BONGOCAT_FREE(ptr)    bongocat_free(ptr)
#endif

#endif  // MEMORY_H