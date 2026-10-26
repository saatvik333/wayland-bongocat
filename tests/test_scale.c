#include "platform/scale.h"
#include "test_helpers.h"

int main(void) {
  // Overflow saturates instead of wrapping.
  TEST_ASSERT(scale_offset_120(INT_MIN, 120) == INT_MIN);
  TEST_ASSERT(scale_offset_120(INT_MAX, 240) == INT_MAX);
  TEST_ASSERT(scale_size_120(INT_MAX, UINT32_MAX) == 0);

  // Fractional scales round sizes up and offsets away from zero.
  TEST_ASSERT(scale_size_120(100, 120) == 100);
  TEST_ASSERT(scale_size_120(100, 144) == 120);
  TEST_ASSERT(scale_size_120(100, 180) == 150);
  TEST_ASSERT(scale_size_120(101, 180) == 152);
  TEST_ASSERT(scale_offset_120(-10, 180) == -15);

  int width;
  int height;
  output_logical_size(2560, 1600, 0, 2, 0, 0, &width, &height);
  TEST_ASSERT(width == 1280 && height == 800);
  output_logical_size(2560, 1600, 1, 2, 0, 0, &width, &height);  // 90 degrees
  TEST_ASSERT(width == 800 && height == 1280);
  // xdg-output's logical size wins over the mode.
  output_logical_size(3840, 2160, 0, 2, 1920, 1080, &width, &height);
  TEST_ASSERT(width == 1920 && height == 1080);
  return 0;
}
