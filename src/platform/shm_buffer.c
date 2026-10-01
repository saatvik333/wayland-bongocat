#define _GNU_SOURCE
#include "platform/shm_buffer.h"

#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client-protocol.h>

static shm_buffer_t *buffers;
static void destroy(shm_buffer_t *buffer) {
  shm_buffer_t **link = &buffers;
  while (*link && *link != buffer) {
    link = &(*link)->next;
  }
  if (*link) {
    *link = buffer->next;
  }
  wl_buffer_destroy(buffer->object);
  munmap(buffer->pixels, buffer->size);
  free(buffer);
}
static void released(void *data, struct wl_buffer *object) {
  (void)object;
  shm_buffer_t *buffer = data;
  buffer->busy = false;
  if (buffer->retired) {
    destroy(buffer);
  }
}
static const struct wl_buffer_listener LISTENER = {.release = released};
shm_buffer_t *shm_buffer_create(struct wl_shm *shm, int width, int height) {
  if (width <= 0 || height <= 0 || width > INT_MAX / 4) {
    return NULL;
  }
  uint64_t wide_size = (uint64_t)width * height * 4;
  if (wide_size > INT_MAX || wide_size > UINT64_C(256) * 1024 * 1024) {
    return NULL;
  }
  size_t size = (size_t)wide_size;
  int fd = memfd_create("bongocat", MFD_CLOEXEC);
  if (fd < 0) {
    return NULL;
  }
  if (ftruncate(fd, (off_t)size) < 0) {
    close(fd);
    return NULL;
  }
  uint8_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (pixels == MAP_FAILED) {
    close(fd);
    return NULL;
  }
  shm_buffer_t *buffer = calloc(1, sizeof(*buffer));
  if (!buffer) {
    munmap(pixels, size);
    close(fd);
    return NULL;
  }
  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int)size);
  buffer->object = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4,
                                             WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);
  buffer->pixels = pixels;
  buffer->size = size;
  buffer->next = buffers;
  buffers = buffer;
  wl_buffer_add_listener(buffer->object, &LISTENER, buffer);
  return buffer;
}
void shm_buffer_retire(shm_buffer_t *buffer) {
  if (!buffer) {
    return;
  }
  buffer->retired = true;
  if (!buffer->busy) {
    destroy(buffer);
  }
}
void shm_buffers_cleanup(void) {
  while (buffers) {
    destroy(buffers);
  }
}
