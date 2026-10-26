#define _GNU_SOURCE
#include "platform/niri.h"

#include "utils/error.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// =============================================================================
// NIRI FOCUS TRACKING
// =============================================================================
//
// Wayland has no protocol for the focused output, and foreign-toplevel focus
// disappears when an empty workspace is focused. niri's event stream reports
// the focused workspace, its output, and whether it holds any window.

#define NIRI_MAX_WORKSPACES 128
#define NIRI_NAME_SIZE      128  // Matches output_ref_t.name.
#define NIRI_KEY_SIZE       32

typedef struct {
  uint64_t id;
  char output[NIRI_NAME_SIZE];
  bool empty;  // No active window
} niri_workspace_t;

typedef struct {
  niri_workspace_t workspace;
  bool has_id, focused, has_window_field;
} niri_member_t;

typedef const char *(*member_handler_t)(const char *key, const char *value,
                                        void *data);

static int stream = -1;
static bool enabled, discarding, changed;
static size_t used;
// Lines beyond this size (large window lists) are discarded unparsed.
static char buffer[65536];
static niri_workspace_t workspaces[NIRI_MAX_WORKSPACES];
static size_t workspace_count;
static uint64_t focused_id;
static bool focus_known;

static const niri_workspace_t *focused_workspace(void) {
  for (size_t i = 0; focus_known && i < workspace_count; i++) {
    if (workspaces[i].id == focused_id) {
      return &workspaces[i];
    }
  }
  return NULL;
}

typedef struct {
  char output[NIRI_NAME_SIZE];  // Empty while unknown
  bool empty;
} focus_t;

static focus_t current_focus(void) {
  const niri_workspace_t *workspace = focused_workspace();
  focus_t focus = {.empty = workspace && workspace->empty};
  if (workspace) {
    memcpy(focus.output, workspace->output, sizeof(focus.output));
  }
  return focus;
}

static void note_change(const focus_t *before) {
  focus_t after = current_focus();
  if (strcmp(before->output, after.output) != 0 ||
      before->empty != after.empty) {
    changed = true;
  }
}

static void forget_state(void) {
  focus_known = false;
  workspace_count = 0;
}

static void disconnect(void) {
  focus_t before = current_focus();
  if (stream >= 0) {
    close(stream);
  }
  stream = -1;
  used = 0;
  discarding = false;
  forget_state();
  note_change(&before);
}

// -----------------------------------------------------------------------------

static const char *skip_space(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
    p++;
  }
  return p;
}

// Copies the string at p, or only skips it when out is NULL, and returns the
// position after it. Escapes keep the escaped character; overlong values
// become empty and never match an output.
static const char *parse_string(const char *p, char *out, size_t size) {
  if (*p != '"') {
    return NULL;
  }
  size_t length = 0;
  bool overflow = false;
  for (p++; *p != '"'; p++) {
    if (*p == '\\') {
      p++;
    }
    if (!*p) {
      return NULL;
    }
    if (out && length + 1 < size) {
      out[length++] = *p;
    } else {
      overflow = true;
    }
  }
  if (out) {
    out[overflow ? 0 : length] = '\0';
  }
  return p + 1;
}

static const char *skip_value(const char *p) {
  int depth = 0;
  do {
    if (*p == '"') {
      p = parse_string(p, NULL, 0);
      if (!p) {
        return NULL;
      }
      continue;
    }
    if (!*p) {
      return NULL;
    }
    if (*p == '{' || *p == '[') {
      depth++;
    } else if (*p == '}' || *p == ']') {
      if (depth == 0) {
        return NULL;
      }
      depth--;
    } else if (depth == 0) {
      size_t length = strcspn(p, ",}] \t\r\n");
      return length ? p + length : NULL;
    }
    p++;
  } while (depth > 0);
  return p;
}

// Returns the position after one object or array element and its separator.
static const char *element_end(const char *p, char close) {
  p = skip_space(p);
  if (*p == ',') {
    return skip_space(p + 1);
  }
  return *p == close ? p : NULL;
}

static const char *parse_object(const char *p, member_handler_t handle,
                                void *data) {
  p = skip_space(p);
  if (*p != '{') {
    return NULL;
  }
  p = skip_space(p + 1);
  while (p && *p != '}') {
    char key[NIRI_KEY_SIZE];
    p = parse_string(p, key, sizeof(key));
    p = p ? skip_space(p) : NULL;
    if (!p || *p != ':') {
      return NULL;
    }
    p = handle(key, skip_space(p + 1), data);
    p = p ? element_end(p, '}') : NULL;
  }
  return p ? p + 1 : NULL;
}

static const char *parse_id(const char *p, uint64_t *id) {
  if (*p < '0' || *p > '9') {
    return NULL;
  }
  errno = 0;
  char *end;
  unsigned long long value = strtoull(p, &end, 10);
  if (errno) {
    return NULL;
  }
  *id = (uint64_t)value;
  return end;
}

// Handles members of workspace objects, WorkspaceActivated and
// WorkspaceActiveWindowChanged.
static const char *workspace_member(const char *key, const char *value,
                                    void *data) {
  niri_member_t *member = data;
  if (strcmp(key, "id") == 0 || strcmp(key, "workspace_id") == 0) {
    member->has_id = true;
    return parse_id(value, &member->workspace.id);
  }
  if (strcmp(key, "output") == 0 && *value == '"') {
    return parse_string(value, member->workspace.output,
                        sizeof(member->workspace.output));
  }
  if (strcmp(key, "is_focused") == 0 || strcmp(key, "focused") == 0) {
    member->focused = strncmp(value, "true", 4) == 0;
  }
  if (strcmp(key, "active_window_id") == 0) {
    member->has_window_field = true;
    member->workspace.empty = strncmp(value, "null", 4) == 0;
  }
  return skip_value(value);
}

static niri_workspace_t *find_workspace(uint64_t id) {
  for (size_t i = 0; i < workspace_count; i++) {
    if (workspaces[i].id == id) {
      return &workspaces[i];
    }
  }
  return NULL;
}

static const char *parse_workspaces(const char *p) {
  if (*p != '[') {
    return NULL;
  }
  focus_known = false;
  workspace_count = 0;
  p = skip_space(p + 1);
  while (p && *p != ']') {
    niri_member_t member = {0};
    p = parse_object(p, workspace_member, &member);
    if (!p || !member.has_id) {
      return NULL;
    }
    if (workspace_count < NIRI_MAX_WORKSPACES) {
      workspaces[workspace_count++] = member.workspace;
      if (member.focused) {
        focus_known = true;
        focused_id = member.workspace.id;
      }
    }
    p = element_end(p, ']');
  }
  return p ? p + 1 : NULL;
}

static const char *workspaces_member(const char *key, const char *value,
                                     void *data) {
  (void)data;
  return strcmp(key, "workspaces") == 0 ? parse_workspaces(value)
                                        : skip_value(value);
}

static const char *parse_member_event(const char *value,
                                      niri_member_t *member) {
  const char *end = parse_object(value, workspace_member, member);
  return end && member->has_id ? end : NULL;
}

static const char *event_member(const char *key, const char *value,
                                void *data) {
  bool *failed = data;
  const char *end;
  niri_member_t member = {0};
  if (strcmp(key, "WorkspacesChanged") == 0) {
    end = parse_object(value, workspaces_member, NULL);
  } else if (strcmp(key, "WorkspaceActivated") == 0) {
    end = parse_member_event(value, &member);
    if (end && member.focused) {
      focus_known = true;
      focused_id = member.workspace.id;
    }
  } else if (strcmp(key, "WorkspaceActiveWindowChanged") == 0) {
    end = parse_member_event(value, &member);
    niri_workspace_t *workspace =
        end ? find_workspace(member.workspace.id) : NULL;
    if (workspace && member.has_window_field) {
      workspace->empty = member.workspace.empty;
    }
  } else if (strcmp(key, "Err") == 0) {
    bongocat_log_warning("niri rejected the event stream request");
    end = NULL;
  } else {
    end = skip_value(value);
  }
  *failed |= !end;
  return end;
}

static void handle_line(const char *line) {
  focus_t before = current_focus();
  bool failed = false;
  if (!parse_object(line, event_member, &failed) || failed) {
    // Unknown state must not pin the overlay to a stale output.
    forget_state();
  }
  note_change(&before);
}

// -----------------------------------------------------------------------------
// Event-stream connection
// -----------------------------------------------------------------------------

static void connect_stream(void) {
  const char *path = getenv("NIRI_SOCKET");
  if (!path || !*path) {
    return;
  }
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  size_t length = strlen(path);
  if (length >= sizeof(address.sun_path)) {
    bongocat_log_warning("NIRI_SOCKET path is too long");
    return;
  }
  memcpy(address.sun_path, path, length + 1);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    bongocat_log_warning("niri socket failed: %s", strerror(errno));
    return;
  }
  if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    bongocat_log_warning("niri IPC unavailable: %s", strerror(errno));
    close(fd);
    return;
  }
  static const char request[] = "\"EventStream\"\n";
  size_t sent = 0;
  while (sent < sizeof(request) - 1) {
    ssize_t count =
        send(fd, request + sent, sizeof(request) - 1 - sent, MSG_NOSIGNAL);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      bongocat_log_warning("niri event stream request failed: %s",
                           strerror(errno));
      close(fd);
      return;
    }
    sent += (size_t)count;
  }
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    close(fd);
    return;
  }
  stream = fd;
  bongocat_log_debug("Following niri focused output");
}

void niri_set_enabled(bool value) {
  if (enabled == value) {
    return;
  }
  enabled = value;
  if (enabled) {
    connect_stream();
  } else {
    disconnect();
  }
}

void niri_poll(void) {
  while (stream >= 0) {
    ssize_t count = read(stream, buffer + used, sizeof(buffer) - 1 - used);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0 && errno == EAGAIN) {
      return;
    }
    if (count <= 0) {
      bongocat_log_warning("niri event stream closed");
      disconnect();
      return;
    }
    size_t end = used + (size_t)count;
    size_t start = 0;
    for (size_t i = used; i < end; i++) {
      if (buffer[i] != '\n') {
        continue;
      }
      buffer[i] = '\0';
      if (!discarding) {
        handle_line(buffer + start);
      }
      discarding = false;
      start = i + 1;
    }
    used = end - start;
    memmove(buffer, buffer + start, used);
    if (used == sizeof(buffer) - 1) {
      discarding = true;
      used = 0;
    }
    buffer[used] = '\0';
  }
}

int niri_fd(void) {
  return stream;
}

const char *niri_focused_output(void) {
  const niri_workspace_t *workspace = focused_workspace();
  return workspace && workspace->output[0] ? workspace->output : NULL;
}

bool niri_focused_empty(void) {
  const niri_workspace_t *workspace = focused_workspace();
  return workspace && workspace->empty;
}

bool niri_take_changed(void) {
  bool result = changed;
  changed = false;
  return result;
}

void niri_cleanup(void) {
  disconnect();
  enabled = false;
  changed = false;
}
