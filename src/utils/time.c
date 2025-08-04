#define _GNU_SOURCE
#include "utils/time.h"
#include <time.h>
#include <sys/time.h>

timestamp_us_t get_current_time_us(void) {
    struct timeval now;
    gettimeofday(&now, NULL);
    return (timestamp_us_t)now.tv_sec * 1000000LL + now.tv_usec;
}
timestamp_ms_t get_current_time_ms(void) {
    return get_current_time_us() / 1000;
}

time_us_t get_uptime_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0) {
        return 0;
    }
    return (time_us_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
time_ms_t get_uptime_ms(void) {
    return get_uptime_us() / 1000;
}