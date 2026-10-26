#ifndef BONGOCAT_CLOCK_H
#define BONGOCAT_CLOCK_H

#include <stdint.h>
#include <time.h>

static inline int64_t monotonic_us(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return ((int64_t)now.tv_sec * 1000000) + (now.tv_nsec / 1000);
}

static inline int64_t monotonic_ms(void) {
  return monotonic_us() / 1000;
}

// Milliseconds until a monotonic microsecond deadline, rounded up, for poll().
// Zero means "no deadline" and yields -1 (wait indefinitely).
static inline int poll_timeout_until(int64_t deadline_us, int64_t now_us) {
  if (!deadline_us) {
    return -1;
  }
  if (deadline_us <= now_us) {
    return 0;
  }
  int64_t remaining = (deadline_us - now_us + 999) / 1000;
  return remaining > INT32_MAX ? INT32_MAX : (int)remaining;
}

#endif  // BONGOCAT_CLOCK_H
