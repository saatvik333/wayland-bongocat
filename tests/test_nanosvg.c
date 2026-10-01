#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "test_helpers.h"

#include <stdbool.h>

static bool fail_reallocation;
static void *test_realloc(void *pointer, size_t size) {
  return fail_reallocation ? NULL : realloc(pointer, size);
}
#define realloc test_realloc
#include <nanosvg.h>
#include <nanosvgrast.h>
#undef realloc

static void test_singular_inverse(void) {
  float transform[6] = {0};
  float inverse[6] = {9, 9, 9, 9, 9, 9};
  nsvg__xformInverse(inverse, transform);
  TEST_ASSERT(inverse[0] == 1 && inverse[3] == 1);
  TEST_ASSERT(inverse[1] == 0 && inverse[2] == 0 && inverse[4] == 0 &&
              inverse[5] == 0);
  for (size_t i = 0; i < 6; i++) {
    TEST_ASSERT(transform[i] == 0);
  }
}
static void test_empty_bounds(void) {
  NSVGshape shape = {0};
  float identity[6];
  float bounds[4] = {9, 9, 9, 9};
  nsvg__xformIdentity(identity);
  nsvg__getLocalBounds(bounds, &shape, identity);
  for (size_t i = 0; i < 4; i++) {
    TEST_ASSERT(bounds[i] == 0);
  }
}
static void test_invalid_point_count(void) {
  NSVGrasterizer rasterizer = {.npoints = -1};
  nsvg__addPathPoint(&rasterizer, 0, 0, 0);
  TEST_ASSERT(rasterizer.npoints == -1 && rasterizer.points == NULL);
}
static void test_failed_point_growth(void) {
  NSVGpoint *points = calloc(1, sizeof(*points));
  TEST_ASSERT(points != NULL);
  NSVGrasterizer rasterizer = {.points = points, .npoints = 1, .cpoints = 1};
  fail_reallocation = true;
  nsvg__addPathPoint(&rasterizer, 1, 1, 0);
  TEST_ASSERT(rasterizer.points == points);
  TEST_ASSERT(rasterizer.npoints == 1 && rasterizer.cpoints == 1);
  nsvg__appendPathPoint(&rasterizer, (NSVGpoint){.x = 2, .y = 2});
  TEST_ASSERT(rasterizer.points == points);
  TEST_ASSERT(rasterizer.npoints == 1 && rasterizer.cpoints == 1);
  fail_reallocation = false;
  nsvg__appendPathPoint(&rasterizer, (NSVGpoint){.x = 2, .y = 2});
  TEST_ASSERT(rasterizer.npoints == 2 && rasterizer.cpoints >= 2);
  free(rasterizer.points);
}
int main(int argc, char **argv) {
  (void)argc;
  if (argv[1] && strcmp(argv[1], "bounds") == 0) {
    test_empty_bounds();
  } else if (argv[1] && strcmp(argv[1], "points") == 0) {
    test_invalid_point_count();
  } else {
    test_singular_inverse();
    test_empty_bounds();
    test_invalid_point_count();
    test_failed_point_growth();
  }
  return 0;
}
