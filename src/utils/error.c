#define _POSIX_C_SOURCE 200809L
#include "utils/error.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static bool debug_enabled;

void bongocat_log_set_debug(bool enabled) {
  debug_enabled = enabled;
}

static void write_line(FILE *stream, const char *level, const char *message) {
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  struct tm local = {0};
  localtime_r(&now.tv_sec, &local);
  char stamp[32];
  strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
  fprintf(stream, "[%s.%03ld] %s: %s\n", stamp, now.tv_nsec / 1000000L, level,
          message);
  fflush(stream);
}

// Formatting stays inside each variadic function: static analyzers lose
// va_list initialization across calls. Clang's analyzer also does not model
// __builtin_c23_va_start, which C23 va_start expands to; the legacy builtin
// has the same contract here.
#define LOG_FUNCTION(name, stream, level, condition)   \
  void name(const char *format, ...) {                 \
    if (!(condition)) {                                \
      return;                                          \
    }                                                  \
    char message[1024];                                \
    va_list args;                                      \
    __builtin_va_start(args, format);                  \
    vsnprintf(message, sizeof(message), format, args); \
    va_end(args);                                      \
    write_line(stream, level, message);                \
  }

LOG_FUNCTION(bongocat_log_error, stderr, "ERROR", true)
LOG_FUNCTION(bongocat_log_warning, stderr, "WARNING", true)
LOG_FUNCTION(bongocat_log_info, stdout, "INFO", true)
LOG_FUNCTION(bongocat_log_debug, stdout, "DEBUG", debug_enabled)

const char *bongocat_error_string(bongocat_error_t error) {
  switch (error) {
  case BONGOCAT_SUCCESS:
    return "Success";
  case BONGOCAT_ERROR_MEMORY:
    return "Memory allocation error";
  case BONGOCAT_ERROR_FILE_IO:
    return "File I/O error";
  case BONGOCAT_ERROR_WAYLAND:
    return "Wayland error";
  case BONGOCAT_ERROR_CONFIG:
    return "Configuration error";
  case BONGOCAT_ERROR_INPUT:
    return "Input error";
  }
  return "Unknown error";
}
