#define _POSIX_C_SOURCE 200809L
#include "platform/hyprland.h"
#include "platform/wayland.h"
#include "test_helpers.h"

#include <time.h>
output_ref_t outputs[MAX_OUTPUTS];
size_t output_count;
void wayland_request_redraw(void) {}
static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
int main(void) {
  char buffer[128];
  const char *success[] = {"/usr/bin/printf", "hello", NULL};
  TEST_ASSERT(safe_exec_read(success, buffer, sizeof(buffer)) == 5);
  TEST_ASSERT(strcmp(buffer, "hello") == 0);
  const char *failure[] = {"/usr/bin/false", NULL};
  TEST_ASSERT(safe_exec_read(failure, buffer, sizeof(buffer)) == -1);
  const char *missing[] = {"/missing-executable", NULL};
  TEST_ASSERT(safe_exec_read(missing, buffer, sizeof(buffer)) == -1);
  const char *timeout[] = {"/usr/bin/sleep", "5", NULL};
  int64_t begin = now_ms();
  TEST_ASSERT(safe_exec_read(timeout, buffer, sizeof(buffer)) == -1);
  TEST_ASSERT(now_ms() - begin < 1500);
  const char *truncated[] = {"/usr/bin/printf", "more-than-four-bytes", NULL};
  TEST_ASSERT(safe_exec_read(truncated, buffer, 4) == -1);
  return 0;
}
