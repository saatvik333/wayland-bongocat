#ifndef BONGOCAT_SCALE_H
#define BONGOCAT_SCALE_H

#include <stdbool.h>
#include <stdint.h>

static inline int scale_size_120(int logical, uint32_t scale_120) {
  if (logical <= 0 || scale_120 == 0)
    return 0;
  return (int)(((int64_t)logical * scale_120 + 119) / 120);
}

static inline int scale_offset_120(int logical, uint32_t scale_120) {
  if (logical >= 0)
    return scale_size_120(logical, scale_120);
  return -scale_size_120(-logical, scale_120);
}

static inline void output_logical_size(int raw_width, int raw_height,
                                       int transform, int integer_scale,
                                       int xdg_width, int xdg_height,
                                       int *width, int *height) {
  if (xdg_width > 0 && xdg_height > 0) {
    *width = xdg_width;
    *height = xdg_height;
    return;
  }
  bool rotated =
      transform == 1 || transform == 3 || transform == 5 || transform == 7;
  int w = rotated ? raw_height : raw_width;
  int h = rotated ? raw_width : raw_height;
  int scale = integer_scale > 0 ? integer_scale : 1;
  *width = w > 0 ? (w + scale - 1) / scale : 0;
  *height = h > 0 ? (h + scale - 1) / scale : 0;
}

#endif
