// Include the implementation to exercise the helper's actual poll loop seam.
#include "../src/platform/input.c"
#include "test_helpers.h"

static void test_disconnect(short event) {
  active_device_t devices[MAX_ACTIVE_DEVICES] = {0};
  struct pollfd pfds[MAX_ACTIVE_DEVICES];
  int mapping[MAX_ACTIVE_DEVICES];
  int healthy[2], disconnected[2];
  bool found = true;
  TEST_ASSERT(pipe(healthy) == 0);
  TEST_ASSERT(pipe(disconnected) == 0);
  for (int i = 0; i < MAX_ACTIVE_DEVICES; i++) {
    devices[i].fd = -1;
  }
  // Nonadjacent slots also verify the poll-index to device-slot mapping.
  devices[2].fd = healthy[0];
  devices[7].fd = disconnected[event == POLLERR ? 1 : 0];
  if (event == POLLERR) {
    close(disconnected[0]);
  } else {
    close(disconnected[1]);
    if (event == POLLNVAL) {
      close(disconnected[0]);
    }
  }
  nfds_t count = prepare_device_poll(devices, pfds, mapping);
  TEST_ASSERT(count == 2);
  TEST_ASSERT(poll(pfds, count, 0) == 1);
  TEST_ASSERT(pfds[1].revents & event);
  read_ready_devices(devices, pfds, count, mapping, &found, 2, 0);
  count = prepare_device_poll(devices, pfds, mapping);
  // Leaving a terminal fd here causes an immediate wake on every iteration.
  TEST_ASSERT(count == 1);
  TEST_ASSERT(poll(pfds, count, 20) == 0);
  TEST_ASSERT(!found);
  TEST_ASSERT(devices[7].fd == -1);
  TEST_ASSERT(devices[2].fd == healthy[0]);
  close(healthy[0]);
  close(healthy[1]);
}

int main(void) {
  test_disconnect(POLLHUP);
  test_disconnect(POLLERR);
  test_disconnect(POLLNVAL);
  puts("Disconnected input descriptors cannot spin the poll loop.");
  return 0;
}
