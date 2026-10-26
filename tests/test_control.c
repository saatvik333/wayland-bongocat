#define _GNU_SOURCE
#include "config/config_watcher.h"
#include "core/control.h"
#include "test_helpers.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void sleep_ms(int ms) {
  struct timespec duration = {.tv_sec = ms / 1000,
                              .tv_nsec = (long)(ms % 1000) * 1000000};
  while (nanosleep(&duration, &duration) < 0 && errno == EINTR) {}
}

// Processes events, waits past the settle delay, and reports whether a reload
// became due.
static bool reload_after_settle(config_watcher_t *watcher) {
  bool due = config_watcher_process(watcher);
  sleep_ms(350);
  return config_watcher_process(watcher) || due;
}

static void test_watcher(const char *path, const char *other) {
  config_watcher_t watcher;
  write_file(path, "fps=30\n");
  TEST_ASSERT(config_watcher_init(&watcher, path));
  // Other files in the directory are ignored.
  write_file(other, "fps=30\n");
  TEST_ASSERT(!reload_after_settle(&watcher));
  TEST_ASSERT(!config_watcher_deadline(&watcher));

  // Reloads wait until writes settle.
  for (int i = 0; i < 4; i++) {
    write_file(path, "fps=30\n");
    TEST_ASSERT(!config_watcher_process(&watcher));
    TEST_ASSERT(config_watcher_deadline(&watcher));
    sleep_ms(100);
  }
  TEST_ASSERT(!config_watcher_process(&watcher));
  sleep_ms(250);
  TEST_ASSERT(config_watcher_process(&watcher));
  TEST_ASSERT(!config_watcher_process(&watcher));

  // Atomic replacement, removal and recreation.
  write_file(other, "fps=60\n");
  TEST_ASSERT(rename(other, path) == 0);
  TEST_ASSERT(reload_after_settle(&watcher));
  TEST_ASSERT(unlink(path) == 0);
  TEST_ASSERT(reload_after_settle(&watcher));
  write_file(path, "fps=1\n");
  TEST_ASSERT(reload_after_settle(&watcher));
  config_watcher_cleanup(&watcher);

  // A symlink whose target lives in another directory.
  char directory[512];
  char target[512];
  char replacement[512];
  snprintf(directory, sizeof(directory), "%s.d", other);
  snprintf(target, sizeof(target), "%s.d/target", other);
  snprintf(replacement, sizeof(replacement), "%s.d/replacement", other);
  TEST_ASSERT(mkdir(directory, 0700) == 0);
  write_file(target, "fps=30\n");
  TEST_ASSERT(unlink(path) == 0);
  TEST_ASSERT(symlink(target, path) == 0);
  TEST_ASSERT(config_watcher_init(&watcher, path));
  write_file(target, "fps=60\n");
  TEST_ASSERT(reload_after_settle(&watcher));
  write_file(replacement, "fps=90\n");
  TEST_ASSERT(rename(replacement, target) == 0);
  TEST_ASSERT(reload_after_settle(&watcher));
  TEST_ASSERT(unlink(target) == 0);
  TEST_ASSERT(reload_after_settle(&watcher));
  // Nothing reports the target's return; the retry deadline must arrive.
  write_file(target, "fps=120\n");
  int64_t retry = config_watcher_deadline(&watcher);
  TEST_ASSERT(retry && retry == config_watcher_deadline(&watcher));
  TEST_ASSERT(!reload_after_settle(&watcher));
  sleep_ms(700);
  TEST_ASSERT(reload_after_settle(&watcher));
  TEST_ASSERT(!config_watcher_deadline(&watcher));
  // Retargeting the symlink detaches the old inode.
  write_file(other, "fps=10\n");
  TEST_ASSERT(unlink(path) == 0);
  TEST_ASSERT(symlink(other, path) == 0);
  TEST_ASSERT(reload_after_settle(&watcher));
  write_file(target, "fps=20\n");
  TEST_ASSERT(!reload_after_settle(&watcher));
  write_file(other, "fps=30\n");
  TEST_ASSERT(reload_after_settle(&watcher));
  config_watcher_cleanup(&watcher);
  unlink(path);
  unlink(target);
  rmdir(directory);
}

static int handler(const char *request, char *reply, size_t size) {
  snprintf(reply, size, "%s", request);
  return strcmp(request, "status") != 0;
}

static void test_instance(const char *directory) {
  TEST_ASSERT(setenv("XDG_RUNTIME_DIR", directory, 1) == 0);
  TEST_ASSERT(control_request("status") == 2);  // Nothing running
  TEST_ASSERT(instance_lock() == 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/bongocat.pid", directory);
  struct stat before;
  struct stat after;
  TEST_ASSERT(stat(path, &before) == 0);
  pid_t child = fork();
  TEST_ASSERT(child >= 0);
  if (!child) {
    _exit(instance_lock() == INSTANCE_LOCKED ? 0 : 1);
  }
  int status;
  TEST_ASSERT(waitpid(child, &status, 0) == child);
  TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  TEST_ASSERT(stat(path, &after) == 0 && before.st_ino == after.st_ino &&
              before.st_size == after.st_size);

  TEST_ASSERT(control_start() == 0);
  for (const char *request = "status"; request;
       request = strcmp(request, "status") == 0 ? "bogus" : NULL) {
    child = fork();
    TEST_ASSERT(child >= 0);
    if (!child) {
      _exit(control_request(request));
    }
    // Serve until the client has its answer.
    while (waitpid(child, &status, WNOHANG) == 0) {
      control_process(handler);
      sleep_ms(5);
    }
    TEST_ASSERT(WIFEXITED(status));
    TEST_ASSERT(WEXITSTATUS(status) == (strcmp(request, "status") ? 1 : 0));
  }
  control_cleanup();
  instance_unlock();
  // The lock file keeps its inode across instances.
  TEST_ASSERT(instance_lock() == 0);
  TEST_ASSERT(stat(path, &after) == 0 && before.st_ino == after.st_ino);
  instance_unlock();
  unlink(path);
}

int main(void) {
  char directory[] = "/tmp/bongocat-control-XXXXXX";
  TEST_ASSERT(mkdtemp(directory));
  char path[512];
  char other[512];
  snprintf(path, sizeof(path), "%s/config", directory);
  snprintf(other, sizeof(other), "%s/other", directory);
  test_watcher(path, other);
  test_instance(directory);
  unlink(path);
  unlink(other);
  TEST_ASSERT(rmdir(directory) == 0);
  return 0;
}
