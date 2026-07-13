#include "../include/platform/fullscreen.h"

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
  CHECK(fullscreen_toplevel_relevant(true, true, true));
  CHECK(!fullscreen_toplevel_relevant(true, true, false));
  CHECK(!fullscreen_toplevel_relevant(true, false, true));
  CHECK(fullscreen_toplevel_relevant(false, false, true));
  CHECK(!fullscreen_toplevel_relevant(false, false, false));
  return failed != 0;
}
