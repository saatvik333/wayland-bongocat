#ifndef BONGOCAT_TIME_H
#define BONGOCAT_TIME_H

#include <time.h>
#include <stdint.h>

typedef int64_t timestamp_us_t;
typedef int64_t timestamp_ms_t;
typedef int64_t time_us_t;
typedef int64_t time_ms_t;
typedef int64_t time_ns_t;

timestamp_us_t get_current_time_us(void);
timestamp_ms_t get_current_time_ms(void);

time_us_t get_uptime_us(void);
time_ms_t get_uptime_ms(void);

#endif // TIME_H