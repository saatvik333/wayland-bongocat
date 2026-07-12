#include "../include/platform/scale.h"

#include <stdio.h>

static int failed;
#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                          \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
      failed++;                                                                  \
    }                                                                            \
  } while (0)

int main(void) {
  CHECK(scale_size_120(100, 120) == 100);
  CHECK(scale_size_120(100, 144) == 120);
  CHECK(scale_size_120(100, 180) == 150);
  CHECK(scale_size_120(100, 240) == 200);
  CHECK(scale_offset_120(-10, 180) == -15);

  int width, height;
  output_logical_size(2560, 1600, 0, 2, 0, 0, &width, &height);
  CHECK(width == 1280 && height == 800);
  output_logical_size(2560, 1600, 1, 2, 0, 0, &width, &height);
  CHECK(width == 800 && height == 1280);
  output_logical_size(3840, 2160, 0, 2, 1920, 1080, &width, &height);
  CHECK(width == 1920 && height == 1080);
  output_logical_size(1920, 1080, 0, 1, 0, 0, &width, &height);
  CHECK(width == 1920 && height == 1080);
  return failed != 0;
}
