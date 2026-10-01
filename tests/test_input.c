#define _GNU_SOURCE
#include "platform/input.h"
#include "test_helpers.h"

#include <stdarg.h>
#include <sys/sysmacros.h>

int __wrap_ioctl(int fd, unsigned long request, ...);
int __wrap_stat(const char *path, struct stat *st);
int __wrap_ioctl(int fd, unsigned long request, ...) {
  va_list args;
  va_start(args, request);
  void *buffer = va_arg(args, void *);
  va_end(args);
  if (fd < 0) {
    errno = ENODEV;
    return -1;
  }
  if (_IOC_NR(request) == _IOC_NR(EVIOCGNAME(256))) {
    snprintf(buffer, _IOC_SIZE(request), "%s",
             fd == 1 ? "Test Keyboard" : "Mouse");
    return 0;
  }
  memset(buffer, 0, _IOC_SIZE(request));
  if (fd == 1) {
    unsigned long *bits = buffer;
    unsigned keys[] = {KEY_A, KEY_Z, KEY_ENTER, KEY_SPACE};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
      bits[keys[i] / (8 * sizeof(unsigned long))] |=
          1UL << (keys[i] % (8 * sizeof(unsigned long)));
  }
  return 0;
}
int __wrap_stat(const char *path, struct stat *st) {
  if (!strcmp(path, "/dev/input/by-id/keyboard") ||
      !strcmp(path, "/dev/input/by-path/keyboard") ||
      !strcmp(path, "/dev/input/event3")) {
    *st = (struct stat){.st_mode = S_IFCHR | 0600, .st_rdev = makedev(13, 67)};
    return 0;
  }
  errno = ENOENT;
  return -1;
}
int main(void) {
  dev_t keyboard = makedev(13, 67);
  TEST_ASSERT(input_device_selected(1, keyboard, NULL, 0, NULL, 0));
  TEST_ASSERT(!input_device_selected(2, makedev(13, 68), NULL, 0, NULL, 0));
  TEST_ASSERT(!input_device_selected(-1, keyboard, NULL, 0, NULL, 0));
  char *names[] = {"Missing Keyboard"};
  TEST_ASSERT(!input_device_selected(1, keyboard, NULL, 0, names, 1));
  char *paths[] = {"/dev/input/by-id/keyboard"};
  TEST_ASSERT(input_device_selected(1, keyboard, paths, 1, NULL, 0));
  paths[0] = "/dev/input/by-path/keyboard";
  TEST_ASSERT(input_device_selected(1, keyboard, paths, 1, NULL, 0));
  TEST_ASSERT(!input_device_selected(2, makedev(13, 68), paths, 1, NULL, 0));
  paths[0] = "/dev/input/missing";
  TEST_ASSERT(!input_device_selected(1, keyboard, paths, 1, NULL, 0));
  names[0] = "Test Keyboard";
  TEST_ASSERT(input_device_selected(1, keyboard, paths, 1, names, 1));
  return 0;
}
