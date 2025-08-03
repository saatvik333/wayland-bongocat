#ifndef BONGOCAT_ERROR_H
#define BONGOCAT_ERROR_H

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

// Error codes
typedef enum {
    BONGOCAT_SUCCESS = 0,
    BONGOCAT_ERROR_MEMORY,
    BONGOCAT_ERROR_FILE_IO,
    BONGOCAT_ERROR_WAYLAND,
    BONGOCAT_ERROR_CONFIG,
    BONGOCAT_ERROR_INPUT,
    BONGOCAT_ERROR_ANIMATION,
    BONGOCAT_ERROR_THREAD,
    BONGOCAT_ERROR_INVALID_PARAM
} bongocat_error_t;

// Error handling macros
#define BONGOCAT_CHECK_NULL(ptr, error_code) \
    do { \
        if ((ptr) == NULL) { \
            bongocat_log_error("NULL pointer: %s at %s:%d", #ptr, __FILE__, __LINE__); \
            return (error_code); \
        } \
    } while(0)

#define BONGOCAT_CHECK_ERROR(condition, error_code, message) \
    do { \
        if (condition) { \
            bongocat_log_error("%s at %s:%d", message, __FILE__, __LINE__); \
            return (error_code); \
        } \
    } while(0)

#define BONGOCAT_SAFE_FREE(ptr) \
    do { \
        if (ptr) { \
            free(ptr); \
            ptr = NULL; \
        } \
    } while(0)

// Logging functions
void bongocat_log_error(const char *format, ...);
void bongocat_log_warning(const char *format, ...);
void bongocat_log_info(const char *format, ...);
void bongocat_log_debug(const char *format, ...);

// Error handling initialization
void bongocat_error_init(int enable_debug);
const char* bongocat_error_string(bongocat_error_t error);

#endif // ERROR_H