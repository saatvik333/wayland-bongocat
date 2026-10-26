#define _POSIX_C_SOURCE 200809L
#include "config/config.h"
#include "test_helpers.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char path[] = "/tmp/bongocat-config-XXXXXX";

static config_t load(const char *text, bool strict, bongocat_error_t expect) {
  write_file(path, text);
  config_t config;
  bongocat_error_t result = config_load(&config, path, strict);
  if (result != expect) {
    fprintf(stderr, "config %s: expected %d, got %d\n", text, expect, result);
    exit(EXIT_FAILURE);
  }
  return config;
}

static void test_defaults(void) {
  // The documented defaults apply without a file.
  config_t config;
  TEST_ASSERT(config_load(&config, NULL, true) == BONGOCAT_SUCCESS);
  TEST_ASSERT(
      config.look.cat_height == 40 && config.look.overlay_height == 50 &&
      config.look.overlay_opacity == 150 && config.look.cat_x_offset == 100 &&
      config.look.cat_y_offset == 10 && config.look.layer == LAYER_TOP &&
      config.look.overlay_position == POSITION_TOP &&
      config.look.cat_align == ALIGN_CENTER);
  TEST_ASSERT(config.fps == 60 && config.keypress_duration == 100 &&
              config.test_animation_duration == 200 &&
              config.enable_hand_mapping && config.follow_active_monitor &&
              !config.num_monitors && !config.enable_debug);
  config_free(&config);

  // A missing explicit file is fatal only when strict.
  TEST_ASSERT(config_load(&config, "/nonexistent/bongocat.conf", true) ==
              BONGOCAT_ERROR_FILE_IO);
  TEST_ASSERT(config_load(&config, "/nonexistent/bongocat.conf", false) ==
              BONGOCAT_SUCCESS);
  TEST_ASSERT(config.fps == 60);
  config_free(&config);
}

static void test_values(void) {
  config_t config = load("# comment\n"
                         "; also a comment\n"
                         "  fps = 30  # inline comment\n"
                         "\t\n"
                         "layer=overlay\n"
                         "overlay_position=bottom\n"
                         "cat_align=left\n"
                         "cat_x_offset=-2147483648\n"
                         "monitor= eDP-1 ,, HDMI-A-1 ,\n"
                         "keyboard_device=/dev/input/by-id/kbd\n"
                         "keyboard_name=Keyboard#1\n"
                         "keyboard_name=Second\n"
                         "enable_scheduled_sleep=1\n"
                         "sleep_begin=22:30\n"
                         "sleep_end=06:05\n",
                         true, BONGOCAT_SUCCESS);
  TEST_ASSERT(config.fps == 30);
  TEST_ASSERT(config.look.layer == LAYER_OVERLAY &&
              config.look.overlay_position == POSITION_BOTTOM &&
              config.look.cat_align == ALIGN_LEFT &&
              config.look.cat_x_offset == -2147483647 - 1);
  TEST_ASSERT(config.num_monitors == 2 &&
              strcmp(config.monitors[0], "eDP-1") == 0 &&
              strcmp(config.monitors[1], "HDMI-A-1") == 0);
  TEST_ASSERT(config.num_keyboard_devices == 1 &&
              strcmp(config.keyboard_devices[0], "/dev/input/by-id/kbd") == 0);
  // '#' only starts a comment at the start of a value or after whitespace.
  TEST_ASSERT(config.num_keyboard_names == 2 &&
              strcmp(config.keyboard_names[0], "Keyboard#1") == 0);
  TEST_ASSERT(config.enable_scheduled_sleep &&
              config.sleep_begin == (22 * 60) + 30 &&
              config.sleep_end == (6 * 60) + 5);
  config_free(&config);

  // A later monitor= replaces the list; an empty one selects automatically.
  config = load("monitor=A\nmonitor=\n", true, BONGOCAT_SUCCESS);
  TEST_ASSERT(config.num_monitors == 0);
  config_free(&config);

  config = load("enable_scheduled_sleep=1\nsleep_begin=08:00\n"
                "sleep_end=08:00\n",
                true, BONGOCAT_SUCCESS);
  TEST_ASSERT(!config.enable_scheduled_sleep);
  config_free(&config);
}

static void test_crlf(void) {
  config_t config = load(
      "fps=30\r\nkeyboard_name=Board\r\n\r\n[monitor:A]\r\ncat_height=90\r\n",
      true, BONGOCAT_SUCCESS);
  TEST_ASSERT(config.fps == 30);
  TEST_ASSERT(strcmp(config.keyboard_names[0], "Board") == 0);
  TEST_ASSERT(config_appearance(&config, "A").cat_height == 90);
  config_free(&config);
}

static void test_invalid_lines(void) {
  const char *invalid[] = {
      "fps=0\n",
      "fps=garbage\n",
      "fps=60junk\n",
      "fps=99999999999\n",
      "layer=garbage\n",
      "enable_debug=2\n",
      "follow_active_monitor=yes\n",
      "idle_frame=5\n",
      "sleep_begin=24:00\n",
      "sleep_begin=22:30junk\n",
      "keyboard_device=/dev/input/../shadow\n",
      "keyboard_device=/etc/passwd\n",
      "unknown_setting=1\n",
      "invalid line\n",
      "[monitor:A]\nfps=30\n",
      "[monitor:A]\ncat_height=999\n",
      "[monitr:A]\n",
      "[monitor:]\n",
  };
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    config_t config = load(invalid[i], true, BONGOCAT_ERROR_CONFIG);
    TEST_ASSERT(!config.overrides && !config.fps);  // Left empty
  }

  // Without strict, invalid lines are skipped and ranges clamp.
  config_t config = load("fps=garbage\nlayer=garbage\nenable_debug=2\n"
                         "keyboard_device=/etc/passwd\ncat_height=999\n"
                         "overlay_opacity=-5\n",
                         false, BONGOCAT_SUCCESS);
  TEST_ASSERT(config.fps == 60 && config.look.layer == LAYER_TOP &&
              !config.enable_debug && !config.num_keyboard_devices);
  TEST_ASSERT(config.look.cat_height == 200 &&
              config.look.overlay_opacity == 0);
  config_free(&config);
}

static void test_obsolete_keys(void) {
  // Older configs keep loading, even strictly.
  config_t config = load("enable_antialiasing=1\nhotplug_scan_interval=30\n",
                         true, BONGOCAT_SUCCESS);
  config_free(&config);
}

static void test_monitor_sections(void) {
  config_t config = load("cat_height=40\n"
                         "[monitor:A]\n"
                         "cat_height=80\n"
                         "mirror_x=1\n"
                         "[monitor: B ]\n"
                         "layer=bottom\n"
                         "[global]\n"
                         "overlay_height=60\n"
                         "[monitor:A]\n"
                         "overlay_opacity=0\n",
                         true, BONGOCAT_SUCCESS);
  appearance_t a = config_appearance(&config, "A");
  appearance_t b = config_appearance(&config, "B");
  appearance_t other = config_appearance(&config, "C");
  TEST_ASSERT(a.cat_height == 80 && a.mirror_x && a.overlay_opacity == 0);
  TEST_ASSERT(a.overlay_height == 60 && a.layer == LAYER_TOP);
  TEST_ASSERT(b.layer == LAYER_BOTTOM && b.cat_height == 40);
  TEST_ASSERT(other.cat_height == 40 && !other.mirror_x &&
              other.overlay_opacity == 150 && other.overlay_height == 60);
  TEST_ASSERT(config_appearance(&config, NULL).cat_height == 40);
  config_free(&config);

  // Keys below a broken header must not modify the previous section.
  config = load("[monitor:A]\ncat_height=60\n[monitr:B]\ncat_height=150\n"
                "fps=30\n",
                false, BONGOCAT_SUCCESS);
  TEST_ASSERT(config_appearance(&config, "A").cat_height == 60);
  TEST_ASSERT(config.look.cat_height == 40 && config.fps == 60);
  config_free(&config);
}

int main(void) {
  int fd = mkstemp(path);
  TEST_ASSERT(fd >= 0);
  close(fd);
  test_defaults();
  test_values();
  test_crlf();
  test_invalid_lines();
  test_obsolete_keys();
  test_monitor_sections();
  unlink(path);
  return 0;
}
