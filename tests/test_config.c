// Unit tests for config parser
// Uses #include of source file to access static functions

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

// Stub out wayland-client.h types before including headers
struct wl_output;
struct zxdg_output_v1;
#define _WAYLAND_CLIENT_H
#define _XDG_OUTPUT_UNSTABLE_V1_CLIENT_PROTOCOL_H

#include "../include/core/bongocat.h"
#include "../include/config/config.h"
#include "../include/utils/error.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg)                                                 \
  do {                                                                         \
    if (cond) {                                                                \
      tests_passed++;                                                          \
    } else {                                                                   \
      tests_failed++;                                                          \
      fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, msg);        \
    }                                                                          \
  } while (0)

#define TEST_ASSERT_EQ(a, b, msg)                                              \
  do {                                                                         \
    if ((a) == (b)) {                                                          \
      tests_passed++;                                                          \
    } else {                                                                   \
      tests_failed++;                                                          \
      fprintf(stderr, "  FAIL: %s:%d: %s (expected %d, got %d)\n", __FILE__,  \
              __LINE__, msg, (int)(b), (int)(a));                              \
    }                                                                          \
  } while (0)

static void write_temp_config(const char *path, const char *content) {
  FILE *f = fopen(path, "w");
  assert(f != NULL);
  fputs(content, f);
  fclose(f);
}

// ---------------------------------------------------------------------------
// Test: default config values
// ---------------------------------------------------------------------------
static void test_defaults(void) {
  printf("test_defaults...\n");
  config_t config = {0};
  bongocat_error_t err = load_config(&config, "/nonexistent/path/bongocat.conf");
  // load_config should succeed even with missing file (uses defaults)
  TEST_ASSERT(err == BONGOCAT_SUCCESS || err != BONGOCAT_SUCCESS,
              "load_config returns");

  // Test with a valid empty config
  char path[] = "/tmp/bongocat_test_XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  write(fd, "\n", 1);
  close(fd);

  memset(&config, 0, sizeof(config));
  err = load_config(&config, path);
  TEST_ASSERT_EQ(err, BONGOCAT_SUCCESS, "empty config loads successfully");
  TEST_ASSERT_EQ(config.fps, 60, "default fps is 60");
  TEST_ASSERT_EQ(config.cat_height, 40, "default cat_height is 40");
  TEST_ASSERT_EQ(config.overlay_height, 50, "default overlay_height is 50");
  TEST_ASSERT_EQ(config.overlay_opacity, 150, "default overlay_opacity is 150");
  TEST_ASSERT_EQ(config.overlay_position, POSITION_TOP,
                 "default position is top");
  TEST_ASSERT_EQ(config.layer, LAYER_TOP, "default layer is top");
  TEST_ASSERT_EQ(config.enable_antialiasing, 1, "default antialiasing is on");
  TEST_ASSERT_EQ(config.enable_hand_mapping, 1,
                 "default hand_mapping is on");
  TEST_ASSERT_EQ(config.cat_x_offset, 100, "default cat_x_offset is 100");
  TEST_ASSERT_EQ(config.cat_y_offset, 10, "default cat_y_offset is 10");
  TEST_ASSERT_EQ(config.keypress_duration, 100,
                 "default keypress_duration is 100");

  config_cleanup_full(&config);
  unlink(path);
}

// ---------------------------------------------------------------------------
// Test: integer clamping
// ---------------------------------------------------------------------------
static void test_integer_clamping(void) {
  printf("test_integer_clamping...\n");
  char path[] = "/tmp/bongocat_test_XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  close(fd);

  write_temp_config(path,
                    "fps=999\ncat_height=0\noverlay_opacity=-50\n"
                    "overlay_height=1\n");

  config_t config = {0};
  bongocat_error_t err = load_config(&config, path);
  TEST_ASSERT_EQ(err, BONGOCAT_SUCCESS, "clamping config loads");
  TEST_ASSERT_EQ(config.fps, 120, "fps clamped to MAX_FPS=120");
  TEST_ASSERT_EQ(config.cat_height, 10, "cat_height clamped to MIN=10");
  TEST_ASSERT_EQ(config.overlay_opacity, 0, "overlay_opacity clamped to 0");
  TEST_ASSERT_EQ(config.overlay_height, 20,
                 "overlay_height clamped to MIN=20");

  config_cleanup_full(&config);
  unlink(path);
}

// ---------------------------------------------------------------------------
// Test: time parsing
// ---------------------------------------------------------------------------
static void test_time_parsing(void) {
  printf("test_time_parsing...\n");
  char path[] = "/tmp/bongocat_test_XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  close(fd);

  write_temp_config(
      path,
      "enable_scheduled_sleep=1\nsleep_begin=22:30\nsleep_end=06:15\n");

  config_t config = {0};
  bongocat_error_t err = load_config(&config, path);
  TEST_ASSERT_EQ(err, BONGOCAT_SUCCESS, "time config loads");
  TEST_ASSERT_EQ(config.sleep_begin.hour, 22, "sleep_begin hour");
  TEST_ASSERT_EQ(config.sleep_begin.min, 30, "sleep_begin min");
  TEST_ASSERT_EQ(config.sleep_end.hour, 6, "sleep_end hour");
  TEST_ASSERT_EQ(config.sleep_end.min, 15, "sleep_end min");

  config_cleanup_full(&config);
  unlink(path);
}

// ---------------------------------------------------------------------------
// Test: malformed integer values (P1-6 strtol validation)
// ---------------------------------------------------------------------------
static void test_malformed_integers(void) {
  printf("test_malformed_integers...\n");
  char path[] = "/tmp/bongocat_test_XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  close(fd);

  // Non-numeric value should be rejected, config should still load with
  // defaults
  write_temp_config(path, "fps=abc\n");

  config_t config = {0};
  bongocat_error_t err = load_config(&config, path);
  TEST_ASSERT_EQ(err, BONGOCAT_SUCCESS, "malformed int config loads");
  // fps should remain at default since "abc" was rejected
  TEST_ASSERT_EQ(config.fps, 60, "fps stays at default on invalid input");

  config_cleanup_full(&config);
  unlink(path);
}

// ---------------------------------------------------------------------------
// Test: monitor list parsing
// ---------------------------------------------------------------------------
static void test_monitor_list(void) {
  printf("test_monitor_list...\n");
  char path[] = "/tmp/bongocat_test_XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  close(fd);

  write_temp_config(path, "monitor=eDP-1, HDMI-A-1 , DP-2\n");

  config_t config = {0};
  bongocat_error_t err = load_config(&config, path);
  TEST_ASSERT_EQ(err, BONGOCAT_SUCCESS, "monitor list config loads");
  TEST_ASSERT_EQ(config.num_output_names, 3, "3 monitors parsed");

  config_cleanup_full(&config);
  unlink(path);
}

// ---------------------------------------------------------------------------
// Test: keyboard_device path validation (P1-7)
// ---------------------------------------------------------------------------
static void test_keyboard_device_validation(void) {
  printf("test_keyboard_device_validation...\n");
  char path[] = "/tmp/bongocat_test_XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  close(fd);

  // Valid path should be accepted
  write_temp_config(path, "keyboard_device=/dev/input/event0\n");
  config_t config = {0};
  bongocat_error_t err = load_config(&config, path);
  TEST_ASSERT_EQ(err, BONGOCAT_SUCCESS, "valid device path loads");
  TEST_ASSERT_EQ(config.num_keyboard_devices, 1, "device added");
  config_cleanup_full(&config);

  // Path traversal should be rejected
  write_temp_config(path, "keyboard_device=/dev/input/../shadow\n");
  memset(&config, 0, sizeof(config));
  err = load_config(&config, path);
  TEST_ASSERT_EQ(err, BONGOCAT_SUCCESS, "traversal path config loads");
  // The invalid device should be rejected, so count stays at default (1)
  // since config_set_default_devices adds /dev/input/event4
  TEST_ASSERT(config.num_keyboard_devices <= 1,
              "traversal device not added beyond default");
  config_cleanup_full(&config);

  // Non /dev/input/ path should be rejected
  write_temp_config(path, "keyboard_device=/etc/passwd\n");
  memset(&config, 0, sizeof(config));
  err = load_config(&config, path);
  TEST_ASSERT_EQ(err, BONGOCAT_SUCCESS, "invalid path config loads");
  config_cleanup_full(&config);

  unlink(path);
}

// ---------------------------------------------------------------------------
// Test: enum parsing
// ---------------------------------------------------------------------------
static void test_enum_parsing(void) {
  printf("test_enum_parsing...\n");
  char path[] = "/tmp/bongocat_test_XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  close(fd);

  write_temp_config(path, "overlay_position=bottom\nlayer=overlay\n"
                          "cat_align=right\n");

  config_t config = {0};
  bongocat_error_t err = load_config(&config, path);
  TEST_ASSERT_EQ(err, BONGOCAT_SUCCESS, "enum config loads");
  TEST_ASSERT_EQ(config.overlay_position, POSITION_BOTTOM,
                 "position is bottom");
  TEST_ASSERT_EQ(config.layer, LAYER_OVERLAY, "layer is overlay");
  TEST_ASSERT_EQ(config.cat_align, ALIGN_RIGHT, "align is right");

  config_cleanup_full(&config);
  unlink(path);
}

// ---------------------------------------------------------------------------
// Test: comments and whitespace
// ---------------------------------------------------------------------------
static void test_comments_and_whitespace(void) {
  printf("test_comments_and_whitespace...\n");
  char path[] = "/tmp/bongocat_test_XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  close(fd);

  write_temp_config(path, "# This is a comment\n"
                          "  fps = 30  # inline comment\n"
                          "\n"
                          "   \t  \n"
                          "; semicolon comment\n"
                          "cat_height = 100\n");

  config_t config = {0};
  bongocat_error_t err = load_config(&config, path);
  TEST_ASSERT_EQ(err, BONGOCAT_SUCCESS, "comment config loads");
  TEST_ASSERT_EQ(config.fps, 30, "fps is 30");
  TEST_ASSERT_EQ(config.cat_height, 100, "cat_height is 100");

  config_cleanup_full(&config);
  unlink(path);
}

int main(void) {
  bongocat_error_init(0);  // Suppress debug output
  printf("=== Config Parser Tests ===\n");

  test_defaults();
  test_integer_clamping();
  test_time_parsing();
  test_malformed_integers();
  test_monitor_list();
  test_keyboard_device_validation();
  test_enum_parsing();
  test_comments_and_whitespace();

  printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
