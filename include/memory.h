#ifndef MEMORY_H
#define MEMORY_H

#include <stdlib.h>
#include <stdint.h>
#include "error.h"

// Memory pool for efficient allocation
typedef struct memory_pool {
    void *data;
    size_t size;
    size_t used;
    size_t alignment;
} memory_pool_t;

// Safe memory allocation functions
void* bongocat_malloc(size_t size);
void* bongocat_calloc(size_t count, size_t size);
void* bongocat_realloc(void *ptr, size_t size);
void bongocat_free(void *ptr);

// Memory pool functions
memory_pool_t* memory_pool_create(size_t size, size_t alignment);
void* memory_pool_alloc(memory_pool_t *pool, size_t size);
void memory_pool_reset(memory_pool_t *pool);
void memory_pool_destroy(memory_pool_t *pool);

// Memory statistics
typedef struct {
    size_t total_allocated;
    size_t current_allocated;
    size_t peak_allocated;
    size_t allocation_count;
    size_t free_count;
} memory_stats_t;

void memory_get_stats(memory_stats_t *stats);
void memory_print_stats(void);

// Memory leak detection (debug builds)
#ifdef DEBUG
#define BONGOCAT_MALLOC(size) bongocat_malloc_debug(size, __FILE__, __LINE__)
#define BONGOCAT_FREE(ptr) bongocat_free_debug(ptr, __FILE__, __LINE__)
void* bongocat_malloc_debug(size_t size, const char *file, int line);
void bongocat_free_debug(void *ptr, const char *file, int line);
void memory_leak_check(void);
#else
#define BONGOCAT_MALLOC(size) bongocat_malloc(size)
#define BONGOCAT_FREE(ptr) bongocat_free(ptr)
#endif

#define LEN_ARRAY(x)  (sizeof(x) / sizeof((x)[0]))

#endif // MEMORY_H