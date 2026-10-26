#ifndef BONGOCAT_SHM_BUFFER_H
#define BONGOCAT_SHM_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct wl_buffer;
struct wl_shm;

// ARGB8888 buffer. A busy buffer is held by the compositor until released;
// retired buffers are destroyed once released.
typedef struct shm_buffer {
  struct wl_buffer *object;
  uint8_t *pixels;
  size_t size;
  bool busy;
  bool retired;
  struct shm_buffer *next;
} shm_buffer_t;

shm_buffer_t *shm_buffer_create(struct wl_shm *shm, int width, int height);
void shm_buffer_retire(shm_buffer_t *buffer);
void shm_buffers_cleanup(void);

#endif  // BONGOCAT_SHM_BUFFER_H
