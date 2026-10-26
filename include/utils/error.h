#ifndef ERROR_H
#define ERROR_H

#include <stdbool.h>

typedef enum {
  BONGOCAT_SUCCESS = 0,
  BONGOCAT_ERROR_MEMORY,
  BONGOCAT_ERROR_FILE_IO,
  BONGOCAT_ERROR_WAYLAND,
  BONGOCAT_ERROR_CONFIG,
  BONGOCAT_ERROR_INPUT,
} bongocat_error_t;

#define BONGOCAT_PRINTF(index) \
  __attribute__((format(printf, (index), (index) + 1)))

// Errors and warnings go to stderr, info and debug to stdout.
void bongocat_log_error(const char *format, ...) BONGOCAT_PRINTF(1);
void bongocat_log_warning(const char *format, ...) BONGOCAT_PRINTF(1);
void bongocat_log_info(const char *format, ...) BONGOCAT_PRINTF(1);
void bongocat_log_debug(const char *format, ...) BONGOCAT_PRINTF(1);
void bongocat_log_set_debug(bool enabled);

[[nodiscard]] const char *bongocat_error_string(bongocat_error_t error);

#endif  // ERROR_H
