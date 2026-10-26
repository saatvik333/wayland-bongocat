// Includes the implementation to exercise the helper's static functions.
#include "../src/platform/input.c"
#include "test_helpers.h"

#include <stdarg.h>
#include <sys/sysmacros.h>

// Linked with --wrap: fd 1 is a keyboard named "Test Keyboard", fd 2 a mouse.
int __wrap_ioctl(int fd, unsigned long request, ...);
int __wrap_stat(const char *path, struct stat *st);
int __wrap_ioctl(int fd, unsigned long request, ...) {
  va_list args;
  va_start(args, request);
  void *buffer = va_arg(args, void *);
  va_end(args);
  if (fd != 1 && fd != 2) {
    errno = ENODEV;
    return -1;
  }
  memset(buffer, 0, _IOC_SIZE(request));
  if (_IOC_NR(request) == _IOC_NR(EVIOCGNAME(0))) {
    snprintf(buffer, _IOC_SIZE(request), "%s",
             fd == 1 ? "Test Keyboard" : "Mouse");
  } else if (fd == 1) {
    unsigned long *bits = buffer;
    const unsigned keys[] = {KEY_A, KEY_Z, KEY_ENTER, KEY_SPACE};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
      bits[keys[i] / BITS_PER_LONG] |= 1UL << (keys[i] % BITS_PER_LONG);
    }
  }
  return 0;
}
int __wrap_stat(const char *path, struct stat *st) {
  if (!strcmp(path, "/dev/input/by-id/keyboard") ||
      !strcmp(path, "/dev/input/event3")) {
    *st = (struct stat){.st_mode = S_IFCHR | 0600, .st_rdev = makedev(13, 67)};
    return 0;
  }
  errno = ENOENT;
  return -1;
}

static void test_selection(void) {
  dev_t keyboard = makedev(13, 67);
  dev_t mouse = makedev(13, 68);
  // Without selectors every keyboard is used.
  TEST_ASSERT(device_selected(1, keyboard, NULL, 0, NULL, 0));
  TEST_ASSERT(!device_selected(2, mouse, NULL, 0, NULL, 0));
  TEST_ASSERT(!device_selected(-1, keyboard, NULL, 0, NULL, 0));
  // Paths match by device number, so links select their device only.
  char *paths[] = {"/dev/input/by-id/keyboard"};
  TEST_ASSERT(device_selected(1, keyboard, paths, 1, NULL, 0));
  TEST_ASSERT(!device_selected(2, mouse, paths, 1, NULL, 0));
  paths[0] = "/dev/input/missing";
  TEST_ASSERT(!device_selected(1, keyboard, paths, 1, NULL, 0));
  // Names match substrings and also select non-keyboards.
  char *names[] = {"Missing"};
  TEST_ASSERT(!device_selected(1, keyboard, NULL, 0, names, 1));
  names[0] = "Keyboard";
  TEST_ASSERT(device_selected(1, keyboard, paths, 1, names, 1));
  names[0] = "Mouse";
  TEST_ASSERT(device_selected(2, mouse, NULL, 0, names, 1));

  unsigned number;
  TEST_ASSERT(event_node("event12", &number) && number == 12);
  TEST_ASSERT(!event_node("event", &number));
  TEST_ASSERT(!event_node("event1x", &number));
  TEST_ASSERT(!event_node("mouse0", &number));
}

static helper_t empty_helper(int socket) {
  helper_t helper = {.socket = socket};
  for (size_t i = 0; i < MAX_DEVICES; i++) {
    helper.devices[i].fd = -1;
  }
  return helper;
}

// A device that hangs up or fails must be closed, or poll would wake
// immediately on every iteration.
static void test_disconnect(short event) {
  int healthy[2];
  int broken[2];
  TEST_ASSERT(pipe(healthy) == 0 && pipe(broken) == 0);
  helper_t helper = empty_helper(-1);
  // Nonadjacent slots also verify the poll-index to slot mapping.
  helper.devices[2].fd = healthy[0];
  helper.devices[7].fd = broken[event == POLLERR ? 1 : 0];
  if (event == POLLERR) {
    close(broken[0]);
  } else {
    close(broken[1]);
    if (event == POLLNVAL) {
      close(broken[0]);
    }
  }
  struct pollfd pfds[MAX_DEVICES];
  int slots[MAX_DEVICES];
  nfds_t count = device_pollfds(&helper, pfds, slots);
  TEST_ASSERT(count == 2 && slots[0] == 2 && slots[1] == 7);
  TEST_ASSERT(poll(pfds, count, 0) == 1 && (pfds[1].revents & event));
  TEST_ASSERT(read_devices(&helper, pfds, slots, count));
  TEST_ASSERT(helper.devices[7].fd == -1 && helper.devices[2].fd == healthy[0]);
  count = device_pollfds(&helper, pfds, slots);
  TEST_ASSERT(count == 1 && poll(pfds, count, 20) == 0);
  close(healthy[0]);
  close(healthy[1]);
}

static void test_messages(void) {
  int sockets[2];
  int keyboard[2];
  TEST_ASSERT(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets) == 0);
  TEST_ASSERT(pipe(keyboard) == 0);
  helper_t helper = empty_helper(sockets[0]);
  helper.devices[3].fd = keyboard[0];
  struct input_event events[] = {
      {.type = EV_KEY, .code = KEY_A, .value = 1},
      {.type = EV_KEY, .code = KEY_L, .value = 2}, // Repeat: ignored
      {.type = EV_KEY, .code = KEY_L, .value = 0}, // Release: ignored
  };
  TEST_ASSERT(write(keyboard[1], events, sizeof(events)) == sizeof(events));
  struct pollfd pfds[MAX_DEVICES];
  int slots[MAX_DEVICES];
  nfds_t count = device_pollfds(&helper, pfds, slots);
  TEST_ASSERT(poll(pfds, count, -1) == 1);
  TEST_ASSERT(read_devices(&helper, pfds, slots, count));
  input_message_t message;
  TEST_ASSERT(recv(sockets[1], &message, sizeof(message), MSG_DONTWAIT) ==
              sizeof(message));
  TEST_ASSERT(message.paws == PAW_LEFT && message.devices == 1 &&
              message.time > 0);

  // The renderer side reads paws and treats end-of-file as helper exit.
  helper_fd = sockets[1];
  helper_pid = -1;
  TEST_ASSERT(send_message(sockets[0], PAW_RIGHT, 1) == 1);
  TEST_ASSERT(send_message(sockets[0], 4, 1) == 1);  // Invalid bits
  int64_t time = 0;
  TEST_ASSERT(input_read(&time) == PAW_RIGHT && time > 0);
  TEST_ASSERT(input_device_count() == 1);
  close(sockets[0]);
  TEST_ASSERT(input_read(&time) == 0);
  TEST_ASSERT(input_fd() == -1 && !input_running());
  close(keyboard[0]);
  close(keyboard[1]);
}

int main(void) {
  test_selection();
  test_disconnect(POLLHUP);
  test_disconnect(POLLERR);
  test_disconnect(POLLNVAL);
  test_messages();
  return 0;
}
