#ifndef BONGOCAT_TEST_HELPERS_H
#define BONGOCAT_TEST_HELPERS_H

#include <stdio.h>
#include <stdlib.h>

#define TEST_ASSERT(condition)                                             \
  do {                                                                     \
    if (!(condition)) {                                                    \
      fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, \
              #condition);                                                 \
      exit(EXIT_FAILURE);                                                  \
    }                                                                      \
  } while (0)

static inline void write_file(const char *path, const char *text) {
  FILE *file = fopen(path, "w");
  TEST_ASSERT(file);
  TEST_ASSERT(fputs(text, file) >= 0);
  TEST_ASSERT(fclose(file) == 0);
}

#endif  // BONGOCAT_TEST_HELPERS_H
