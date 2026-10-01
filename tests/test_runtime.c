#define _GNU_SOURCE
#include "config/config.h"
#include "core/bongocat.h"
#include "core/control.h"
#include "test_helpers.h"

#include <sys/wait.h>
#include <time.h>

static int reloads;
static int diagnostics;
static void report(const config_diagnostic_t *item, void *data) {
  (void)data;
  TEST_ASSERT(item->file && item->line == 1 && item->key &&
              strcmp(item->key, "fps") == 0 && item->severity == CONFIG_ERROR);
  diagnostics++;
}
static void changed(const char *path) {
  (void)path;
  reloads++;
}
static void write_config(const char *path, const char *text) {
  FILE *file = fopen(path, "w");
  TEST_ASSERT(file);
  TEST_ASSERT(fputs(text, file) >= 0);
  TEST_ASSERT(fclose(file) == 0);
}
static void delay(int ms) {
  struct timespec duration = {.tv_sec = ms / 1000,
                              .tv_nsec = (ms % 1000) * 1000000L};
  while (nanosleep(&duration, &duration) < 0 && errno == EINTR) {}
}
static void config_tests(const char *path) {
  config_t config = {0}, effective;
  write_config(path,
               "[monitor:TEST-1]\ncat_height=80\nmirror_x=1\n"
               "[global]\ncat_height=40\nkeyboard_name=unmatched-selector\n");
  TEST_ASSERT(load_config_strict(&config, path) == BONGOCAT_SUCCESS);
  TEST_ASSERT(config.num_keyboard_devices == 0);
  config_for_monitor(&config, "TEST-1", &effective);
  TEST_ASSERT(effective.cat_height == 80 && effective.mirror_x == 1);
  config_for_monitor(&config, "OTHER", &effective);
  TEST_ASSERT(effective.cat_height == 40 && effective.mirror_x == 0);
  config_cleanup_full(&config);
  write_config(path, "fps=garbage\n");
  TEST_ASSERT(load_config_report(&config, path, true, report, NULL) !=
              BONGOCAT_SUCCESS);
  TEST_ASSERT(diagnostics > 0);
  config_cleanup_full(&config);
  const char *invalid[] = {"fps=0\n",
                           "fps=garbage\n",
                           "layer=garbage\n",
                           "[monitor:TEST-1]\nfps=30\n",
                           "[monitor:TEST-1]\ncat_height=999\n",
                           "invalid line\n",
                           "hotplug_scan_interval=-1\n",
                           "idle_frame=7\n",
                           "keyboard_device=/dev/input/../shadow\n"};
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    write_config(path, invalid[i]);
    TEST_ASSERT(load_config_strict(&config, path) != BONGOCAT_SUCCESS);
    config_cleanup_full(&config);
  }
  unlink(path);
  TEST_ASSERT(load_config_strict(&config, path) == BONGOCAT_ERROR_FILE_IO);
  config_cleanup_full(&config);
  write_config(path, "keyboard_device=/dev/input/by-id/keyboard\n"
                     "keyboard_device=/dev/input/by-path/keyboard\n");
  TEST_ASSERT(load_config_strict(&config, path) == BONGOCAT_SUCCESS);
  TEST_ASSERT(config.num_keyboard_devices == 2);
  config_cleanup_full(&config);
}
static void watcher_tests(const char *path, const char *other) {
  ConfigWatcher watcher;
  TEST_ASSERT(config_watcher_init(&watcher, path, changed) == 0);
  config_watcher_start(&watcher);
  write_config(other, "fps=30\n");
  config_watcher_process(&watcher);
  delay(350);
  config_watcher_process(&watcher);
  TEST_ASSERT(reloads == 0);
  for (int i = 0; i < 4; i++) {
    write_config(path, "fps=30\n");
    config_watcher_process(&watcher);
    delay(100);
    TEST_ASSERT(reloads == 0);
  }
  delay(220);
  config_watcher_process(&watcher);
  TEST_ASSERT(reloads == 1);
  write_config(other, "fps=60\n");
  TEST_ASSERT(rename(other, path) == 0);
  config_watcher_process(&watcher);
  delay(350);
  config_watcher_process(&watcher);
  TEST_ASSERT(reloads == 2);
  unlink(path);
  config_watcher_process(&watcher);
  delay(350);
  config_watcher_process(&watcher);
  TEST_ASSERT(reloads == 3);
  write_config(path, "fps=1\n");
  config_watcher_process(&watcher);
  delay(350);
  config_watcher_process(&watcher);
  TEST_ASSERT(reloads == 4);
  config_watcher_cleanup(&watcher);
}
static int handler(const char *request, char *response, size_t size) {
  snprintf(response, size, "%s", request);
  return strcmp(request, "status") != 0;
}
static void instance_tests(const char *directory) {
  TEST_ASSERT(setenv("XDG_RUNTIME_DIR", directory, 1) == 0);
  TEST_ASSERT(instance_lock() == 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/bongocat.pid", directory);
  struct stat before, after;
  TEST_ASSERT(stat(path, &before) == 0);
  pid_t child = fork();
  TEST_ASSERT(child >= 0);
  if (!child)
    _exit(instance_lock() == -2 ? 0 : 1);
  int status;
  TEST_ASSERT(waitpid(child, &status, 0) == child);
  TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  TEST_ASSERT(stat(path, &after) == 0 && before.st_ino == after.st_ino &&
              before.st_size == after.st_size);
  TEST_ASSERT(control_start() == 0);
  child = fork();
  TEST_ASSERT(child >= 0);
  if (!child)
    _exit(control_request("status"));
  delay(30);
  control_process(handler);
  delay(30);
  control_process(handler);
  TEST_ASSERT(waitpid(child, &status, 0) == child);
  TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  control_cleanup();
  instance_unlock();
  TEST_ASSERT(instance_lock() == 0);
  TEST_ASSERT(stat(path, &after) == 0 && before.st_ino == after.st_ino);
  instance_unlock();
  unlink(path);
}
int main(void) {
  char directory[] = "/tmp/bongocat-unit-XXXXXX";
  TEST_ASSERT(mkdtemp(directory));
  char path[512], other[512];
  snprintf(path, sizeof(path), "%s/config", directory);
  snprintf(other, sizeof(other), "%s/other", directory);
  config_tests(path);
  watcher_tests(path, other);
  instance_tests(directory);
  unlink(path);
  unlink(other);
  TEST_ASSERT(rmdir(directory) == 0);
  return 0;
}
