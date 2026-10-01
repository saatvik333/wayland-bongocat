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
#endif
