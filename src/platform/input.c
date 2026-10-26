#define _GNU_SOURCE
#include "platform/input.h"

#include "graphics/paw_frame.h"
#include "utils/clock.h"
#include "utils/error.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_DEVICES      32
#define MAX_SELECTORS    256
#define RESCAN_DELAY_US  100000  // Lets udev finish permissions and links
#define HELPER_SOCKET_FD 3

typedef struct {
  uint32_t paws;
  uint32_t devices;
  int64_t time;  // Monotonic microseconds
} input_message_t;

// =============================================================================
// SETGID PRIVILEGES
// =============================================================================

// Group granted by a setgid install; (gid_t)-1 when not setgid or dropped.
static gid_t privileged_gid = (gid_t)-1;

void input_privilege_init(void) {
  gid_t rgid;
  gid_t egid;
  gid_t sgid;
  if (getresgid(&rgid, &egid, &sgid) != 0 || egid == rgid) {
    return;
  }
  privileged_gid = egid;
  if (setresgid((gid_t)-1, rgid, (gid_t)-1) != 0) {
    _exit(1);
  }
}

static void privilege_raise(void) {
  if (privileged_gid != (gid_t)-1 &&
      setresgid((gid_t)-1, privileged_gid, (gid_t)-1) != 0) {
    // Not fatal: opening devices fails and the cat stays idle.
    bongocat_log_warning("Could not raise input group: %s", strerror(errno));
  }
}

static void privilege_lower(void) {
  if (privileged_gid != (gid_t)-1 &&
      setresgid((gid_t)-1, getgid(), (gid_t)-1) != 0) {
    _exit(1);
  }
}

void input_privilege_drop(void) {
  if (privileged_gid == (gid_t)-1) {
    return;
  }
  gid_t gid = getgid();
  gid_t rgid;
  gid_t egid;
  gid_t sgid;
  if (setresgid(gid, gid, gid) != 0 || getresgid(&rgid, &egid, &sgid) != 0 ||
      rgid != gid || egid != gid || sgid != gid) {
    _exit(1);
  }
  privileged_gid = (gid_t)-1;
}

// =============================================================================
// DEVICE INSPECTION
// =============================================================================

#define BITS_PER_LONG (8 * sizeof(unsigned long))

static bool is_keyboard(int fd) {
  unsigned long bits[(KEY_MAX / BITS_PER_LONG) + 1] = {0};
  if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) {
    return false;
  }
  static const unsigned keys[] = {KEY_A, KEY_Z, KEY_ENTER, KEY_SPACE};
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    if (!(bits[keys[i] / BITS_PER_LONG] & (1UL << (keys[i] % BITS_PER_LONG)))) {
      return false;
    }
  }
  return true;
}

static void device_name(int fd, char *name, size_t size) {
  memset(name, 0, size);
  if (ioctl(fd, EVIOCGNAME(size - 1), name) < 0) {
    name[0] = '\0';
  }
}

// Explicit paths match by device number, so by-id and by-path links work.
// Without selectors every keyboard is used.
static bool device_selected(int fd, dev_t identity, char *const *paths,
                            int num_paths, char *const *names, int num_names) {
  if (!num_paths && !num_names) {
    return is_keyboard(fd);
  }
  for (int i = 0; i < num_paths; i++) {
    struct stat selected;
    if (stat(paths[i], &selected) == 0 && S_ISCHR(selected.st_mode) &&
        selected.st_rdev == identity) {
      return true;
    }
  }
  if (num_names) {
    char name[256];
    device_name(fd, name, sizeof(name));
    for (int i = 0; i < num_names; i++) {
      if (strstr(name, names[i])) {
        return true;
      }
    }
  }
  return false;
}

static bool event_node(const char *name, unsigned *number) {
  char *end;
  if (strncmp(name, "event", 5) != 0 || !name[5]) {
    return false;
  }
  errno = 0;
  unsigned long value = strtoul(name + 5, &end, 10);
  if (errno || *end || value > UINT_MAX) {
    return false;
  }
  *number = (unsigned)value;
  return true;
}

static int compare_unsigned(const void *a, const void *b) {
  unsigned x = *(const unsigned *)a;
  unsigned y = *(const unsigned *)b;
  return (x > y) - (x < y);
}

// Prints the /dev/input/by-id links that resolve to path.
static void print_links(const char *path) {
  DIR *dir = opendir("/dev/input/by-id");
  struct dirent *entry;
  while (dir && (entry = readdir(dir))) {
    char link[PATH_MAX];
    char target[PATH_MAX];
    if (entry->d_name[0] != '.' &&
        snprintf(link, sizeof(link), "/dev/input/by-id/%s", entry->d_name) <
            (int)sizeof(link) &&
        realpath(link, target) && strcmp(target, path) == 0) {
      printf("    keyboard_device=%s\n", link);
    }
  }
  if (dir) {
    closedir(dir);
  }
}

int input_list_devices(void) {
  DIR *dir = opendir("/dev/input");
  if (!dir) {
    fprintf(stderr, "/dev/input: %s\n", strerror(errno));
    return 1;
  }
  unsigned numbers[1024];
  size_t count = 0;
  struct dirent *entry;
  while ((entry = readdir(dir)) && count < sizeof(numbers) / sizeof(*numbers)) {
    count += event_node(entry->d_name, &numbers[count]);
  }
  closedir(dir);
  qsort(numbers, count, sizeof(*numbers), compare_unsigned);
  unsigned keyboards = 0;
  for (size_t i = 0; i < count; i++) {
    char path[64];
    snprintf(path, sizeof(path), "/dev/input/event%u", numbers[i]);
    privilege_raise();
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    privilege_lower();
    if (fd < 0) {
      printf("%s: inaccessible (%s)\n", path, strerror(errno));
      continue;
    }
    char name[256];
    device_name(fd, name, sizeof(name));
    bool keyboard = is_keyboard(fd);
    close(fd);
    printf("%s: %s%s\n", path, name, keyboard ? " [keyboard]" : "");
    if (keyboard) {
      keyboards++;
      printf("    keyboard_name=%s\n", name);
      print_links(path);
    }
  }
  if (!keyboards) {
    fprintf(stderr, "No accessible keyboards; check device ACLs or input group "
                    "membership.\n");
  }
  return keyboards ? 0 : 1;
}

// =============================================================================
// HELPER PROCESS
// =============================================================================

typedef struct {
  int fd;
  dev_t identity;
  char path[64];
} device_t;

typedef struct {
  device_t devices[MAX_DEVICES];
  char *const *paths;
  int num_paths;
  char *const *names;
  int num_names;
  int socket;
} helper_t;

static uint32_t open_devices(const helper_t *helper) {
  uint32_t count = 0;
  for (size_t i = 0; i < MAX_DEVICES; i++) {
    count += helper->devices[i].fd >= 0;
  }
  return count;
}

static bool already_open(const helper_t *helper, dev_t identity) {
  for (size_t i = 0; i < MAX_DEVICES; i++) {
    if (helper->devices[i].fd >= 0 && helper->devices[i].identity == identity) {
      return true;
    }
  }
  return false;
}

static void attach(helper_t *helper, const char *path) {
  struct stat st;
  if (stat(path, &st) < 0 || !S_ISCHR(st.st_mode) ||
      already_open(helper, st.st_rdev)) {
    return;
  }
  int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    return;
  }
  device_t *slot = NULL;
  for (size_t i = 0; !slot && i < MAX_DEVICES; i++) {
    slot = helper->devices[i].fd < 0 ? &helper->devices[i] : NULL;
  }
  if (fstat(fd, &st) < 0 || !S_ISCHR(st.st_mode) ||
      !device_selected(fd, st.st_rdev, helper->paths, helper->num_paths,
                       helper->names, helper->num_names)) {
    close(fd);
    return;
  }
  if (!slot) {
    bongocat_log_warning("Too many input devices, ignoring %s", path);
    close(fd);
    return;
  }
  *slot = (device_t){.fd = fd, .identity = st.st_rdev};
  snprintf(slot->path, sizeof(slot->path), "%s", path);
  bongocat_log_info("Input: attached %s", path);
}

static void scan(helper_t *helper) {
  DIR *dir = opendir("/dev/input");
  if (!dir) {
    return;
  }
  privilege_raise();
  struct dirent *entry;
  while ((entry = readdir(dir))) {
    unsigned number;
    if (event_node(entry->d_name, &number)) {
      char path[64];
      snprintf(path, sizeof(path), "/dev/input/event%u", number);
      attach(helper, path);
    }
  }
  privilege_lower();
  closedir(dir);
}

static void detach(device_t *device, const char *reason) {
  bongocat_log_info("Input: detached %s (%s)", device->path, reason);
  close(device->fd);
  device->fd = -1;
}

// Watches for new event nodes, their permission changes, and the links that
// explicit keyboard_device paths may use. Re-adding an existing watch is a
// no-op, so this also picks up link directories created later.
static void watch_input(int inotify) {
  inotify_add_watch(inotify, "/dev/input",
                    IN_CREATE | IN_ATTRIB | IN_MOVED_TO | IN_ONLYDIR);
  inotify_add_watch(inotify, "/dev/input/by-id",
                    IN_CREATE | IN_MOVED_TO | IN_ONLYDIR);
  inotify_add_watch(inotify, "/dev/input/by-path",
                    IN_CREATE | IN_MOVED_TO | IN_ONLYDIR);
}

// Returns 1 when sent, 0 when the socket is full, -1 when the renderer is gone.
static int send_message(int socket, uint32_t paws, uint32_t devices) {
  input_message_t message = {
      .paws = paws, .devices = devices, .time = monotonic_us()};
  if (send(socket, &message, sizeof(message), MSG_NOSIGNAL | MSG_DONTWAIT) >=
      0) {
    return 1;
  }
  return errno == EAGAIN || errno == EINTR ? 0 : -1;
}

// Reads one batch from each ready device and reports pressed paws. Devices
// that hang up or fail are closed so they cannot wake poll again.
static bool read_devices(helper_t *helper, const struct pollfd *pfds,
                         const int *slots, nfds_t count) {
  for (nfds_t i = 0; i < count; i++) {
    device_t *device = &helper->devices[slots[i]];
    if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
      detach(device, "disconnected");
      continue;
    }
    if (!(pfds[i].revents & POLLIN)) {
      continue;
    }
    struct input_event events[64];
    ssize_t length = read(device->fd, events, sizeof(events));
    if (length <= 0) {
      if (length == 0 || (errno != EAGAIN && errno != EINTR)) {
        detach(device, length ? strerror(errno) : "disconnected");
      }
      continue;
    }
    unsigned paws = 0;
    for (size_t e = 0; e < (size_t)length / sizeof(*events); e++) {
      if (events[e].type == EV_KEY && events[e].value == 1) {
        paws |= paw_for_keycode(events[e].code);
      }
    }
    // A full socket drops the batch: the renderer is busy, not gone.
    if (paws && send_message(helper->socket, paws, open_devices(helper)) < 0) {
      return false;
    }
  }
  return true;
}

static nfds_t device_pollfds(const helper_t *helper, struct pollfd *pfds,
                             int *slots) {
  nfds_t count = 0;
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (helper->devices[i].fd >= 0) {
      pfds[count] =
          (struct pollfd){.fd = helper->devices[i].fd, .events = POLLIN};
      slots[count++] = i;
    }
  }
  return count;
}

static void run_helper(helper_t *helper) {
  int inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (inotify >= 0) {
    watch_input(inotify);
  }
  scan(helper);
  uint32_t reported = UINT32_MAX;
  int64_t rescan_at = 0;
  for (;;) {
    uint32_t count = open_devices(helper);
    if (count != reported) {
      int sent = send_message(helper->socket, 0, count);
      if (sent < 0) {
        break;
      }
      reported = sent ? count : reported;
    }
    struct pollfd pfds[MAX_DEVICES + 2];
    int slots[MAX_DEVICES];
    nfds_t devices = device_pollfds(helper, pfds, slots);
    // Hangup is always reported: the renderer exiting ends the helper.
    pfds[devices] = (struct pollfd){.fd = helper->socket,
                                    .events = reported != count ? POLLOUT : 0};
    pfds[devices + 1] = (struct pollfd){.fd = inotify, .events = POLLIN};
    int ready =
        poll(pfds, devices + 2, poll_timeout_until(rescan_at, monotonic_us()));
    if (ready < 0 && errno != EINTR) {
      break;
    }
    if (pfds[devices].revents & (POLLHUP | POLLERR | POLLNVAL)) {
      break;
    }
    if (pfds[devices + 1].revents & POLLIN) {
      alignas(struct inotify_event) char buffer[4096];
      while (read(inotify, buffer, sizeof(buffer)) > 0) {}
      watch_input(inotify);
      rescan_at = monotonic_us() + RESCAN_DELAY_US;
    }
    if (rescan_at && monotonic_us() >= rescan_at) {
      rescan_at = 0;
      scan(helper);
    }
    if (ready > 0 && !read_devices(helper, pfds, slots, devices)) {
      break;
    }
  }
  for (size_t i = 0; i < MAX_DEVICES; i++) {
    if (helper->devices[i].fd >= 0) {
      close(helper->devices[i].fd);
    }
  }
  if (inotify >= 0) {
    close(inotify);
  }
}

static bool parse_count(const char *text, int *value) {
  char *end;
  errno = 0;
  long parsed = strtol(text, &end, 10);
  if (errno || end == text || *end || parsed < 0 || parsed > MAX_SELECTORS) {
    return false;
  }
  *value = (int)parsed;
  return true;
}

// argv: bongocat --input-helper NUM_PATHS NUM_NAMES PATHS... NAMES...
int input_helper_main(int argc, char **argv) {
  // Only serve a renderer of the same user that spawned this process.
  struct ucred peer;
  socklen_t length = sizeof(peer);
  if (getsockopt(HELPER_SOCKET_FD, SOL_SOCKET, SO_PEERCRED, &peer, &length) <
          0 ||
      peer.uid != getuid() || peer.pid != getppid()) {
    return 1;
  }
  int num_paths;
  int num_names;
  if (argc < 4 || !parse_count(argv[2], &num_paths) ||
      !parse_count(argv[3], &num_names) || argc != 4 + num_paths + num_names) {
    return 1;
  }
  helper_t helper = {.paths = argv + 4,
                     .num_paths = num_paths,
                     .names = argv + 4 + num_paths,
                     .num_names = num_names,
                     .socket = HELPER_SOCKET_FD};
  for (size_t i = 0; i < MAX_DEVICES; i++) {
    helper.devices[i].fd = -1;
  }
  run_helper(&helper);
  return 0;
}

// =============================================================================
// RENDERER SIDE
// =============================================================================

static pid_t helper_pid = -1;
static int helper_fd = -1;
static uint32_t device_count;

bongocat_error_t input_start(const config_t *config) {
  int sockets[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0,
                 sockets) < 0) {
    return BONGOCAT_ERROR_INPUT;
  }
  char paths[16];
  char names[16];
  snprintf(paths, sizeof(paths), "%d", config->num_keyboard_devices);
  snprintf(names, sizeof(names), "%d", config->num_keyboard_names);
  size_t argc =
      4 + (size_t)config->num_keyboard_devices + config->num_keyboard_names;
  char **argv = calloc(argc + 1, sizeof(*argv));
  if (!argv) {
    close(sockets[0]);
    close(sockets[1]);
    return BONGOCAT_ERROR_MEMORY;
  }
  argv[0] = "/proc/self/exe";
  argv[1] = "--input-helper";
  argv[2] = paths;
  argv[3] = names;
  for (int i = 0; i < config->num_keyboard_devices; i++) {
    argv[4 + i] = config->keyboard_devices[i];
  }
  for (int i = 0; i < config->num_keyboard_names; i++) {
    argv[4 + config->num_keyboard_devices + i] = config->keyboard_names[i];
  }
  posix_spawn_file_actions_t actions;
  int error = posix_spawn_file_actions_init(&actions);
  if (!error) {
    error = posix_spawn_file_actions_adddup2(&actions, sockets[1],
                                             HELPER_SOCKET_FD);
    // Descriptors inherited from whoever launched bongocat stay out of the
    // privileged helper.
    if (!error) {
      error = posix_spawn_file_actions_addclosefrom_np(&actions,
                                                       HELPER_SOCKET_FD + 1);
    }
    if (!error) {
      error = posix_spawn(&helper_pid, "/proc/self/exe", &actions, NULL, argv,
                          environ);
    }
    posix_spawn_file_actions_destroy(&actions);
  }
  free((void *)argv);
  close(sockets[1]);
  if (error) {
    close(sockets[0]);
    helper_pid = -1;
    bongocat_log_warning("Cannot start input helper: %s", strerror(error));
    return BONGOCAT_ERROR_INPUT;
  }
  helper_fd = sockets[0];
  return BONGOCAT_SUCCESS;
}

void input_stop(void) {
  if (helper_pid > 0) {
    // The helper holds no state worth a graceful shutdown.
    kill(helper_pid, SIGKILL);
    while (waitpid(helper_pid, NULL, 0) < 0 && errno == EINTR) {}
  }
  if (helper_fd >= 0) {
    close(helper_fd);
  }
  helper_pid = -1;
  helper_fd = -1;
  device_count = 0;
}

bool input_running(void) {
  return helper_pid > 0;
}

int input_fd(void) {
  return helper_fd;
}

uint32_t input_device_count(void) {
  return device_count;
}

unsigned input_read(int64_t *time) {
  unsigned paws = 0;
  while (helper_fd >= 0) {
    input_message_t message;
    ssize_t length = recv(helper_fd, &message, sizeof(message), MSG_DONTWAIT);
    if (length == sizeof(message)) {
      device_count = message.devices;
      if (message.paws && !(message.paws & ~PAW_BOTH)) {
        paws |= message.paws;
        *time = message.time;
      }
    } else if (length < 0 && (errno == EAGAIN || errno == EINTR)) {
      break;
    } else if (length <= 0) {
      // End of file: the helper exited.
      bongocat_log_warning("Input helper exited");
      input_stop();
    }
  }
  return paws;
}
