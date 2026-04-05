#define _POSIX_C_SOURCE 200809L
#include "utils/error.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <time.h>

static atomic_int debug_enabled = 1;

void bongocat_error_init(int enable_debug) {
  atomic_store(&debug_enabled, enable_debug);
}

static void log_timestamp(FILE *stream) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  time_t sec = ts.tv_sec;
  struct tm *tm_info = localtime(&sec);
  char time_str[32];
  strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
  fprintf(stream, "[%s.%03ld] ", time_str, ts.tv_nsec / 1000000L);
}

void bongocat_log_error(const char *format, ...) {
  va_list args;
  char message[1024];
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  log_timestamp(stderr);
  fprintf(stderr, "ERROR: %s\n", message);
  fflush(stderr);
}

void bongocat_log_warning(const char *format, ...) {
  va_list args;
  char message[1024];
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  log_timestamp(stderr);
  fprintf(stderr, "WARNING: %s\n", message);
  fflush(stderr);
}

void bongocat_log_info(const char *format, ...) {
  va_list args;
  char message[1024];
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  log_timestamp(stdout);
  fprintf(stdout, "INFO: %s\n", message);
  fflush(stdout);
}

void bongocat_log_debug(const char *format, ...) {
  if (!atomic_load(&debug_enabled))
    return;

  va_list args;
  char message[1024];
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  log_timestamp(stdout);
  fprintf(stdout, "DEBUG: %s\n", message);
  fflush(stdout);
}

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
  case BONGOCAT_ERROR_ANIMATION:
    return "Animation error";
  case BONGOCAT_ERROR_THREAD:
    return "Thread error";
  case BONGOCAT_ERROR_INVALID_PARAM:
    return "Invalid parameter";
  default:
    return "Unknown error";
  }
}
