#include "platform/fullscreen.h"

#include "core/bongocat.h"
#include "platform/hyprland.h"
#include "platform/outputs.h"
#include "platform/wayland.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <wayland-util.h>
#ifdef __GNUC__
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#endif
#include "wlr-foreign-toplevel-management-v1-client-protocol.h"
#ifdef __GNUC__
#  pragma GCC diagnostic pop
#endif

typedef struct {
  bool fullscreen, activated;
  struct wl_output *outputs[MAX_OUTPUTS];
} window_state_t;
typedef struct {
  struct zwlr_foreign_toplevel_handle_v1 *handle;
  window_state_t pending, committed;
} tracked_t;
static tracked_t tracked[MAX_TOPLEVELS];
static struct zwlr_foreign_toplevel_manager_v1 *manager;
static bool output_events;

void fullscreen_recompute(void) {
  bool fullscreen = false;
  for (size_t i = 0; i < MAX_TOPLEVELS; i++) {
    if (!tracked[i].handle) {
      continue;
    }
    bool on_output = false;
    for (size_t j = 0; j < MAX_OUTPUTS; j++) {
      on_output |= output && tracked[i].committed.outputs[j] == output;
    }
    fullscreen |= tracked[i].committed.fullscreen &&
                  fullscreen_toplevel_relevant(output_events, on_output,
                                               tracked[i].committed.activated);
  }
  if (!manager || !output_events) {
    fullscreen |= hypr_fullscreen_for_output(output);
  }
  atomic_store(&fullscreen_detected, fullscreen);
}
static void state(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                  struct wl_array *values) {
  (void)handle;
  tracked_t *window = data;
  window->pending.fullscreen = window->pending.activated = false;
  uint32_t *value;
  wl_array_for_each(value, values) {
    window->pending.fullscreen |=
        *value == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN;
    window->pending.activated |=
        *value == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED;
  }
}
static void enter(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                  struct wl_output *object) {
  (void)handle;
  tracked_t *window = data;
  output_events = true;
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    if (window->pending.outputs[i] == object) {
      return;
    }
  }
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    if (!window->pending.outputs[i]) {
      window->pending.outputs[i] = object;
      return;
    }
  }
}
static void leave(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                  struct wl_output *object) {
  (void)handle;
  tracked_t *window = data;
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    if (window->pending.outputs[i] == object) {
      window->pending.outputs[i] = NULL;
    }
  }
}
static void done(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle) {
  (void)handle;
  tracked_t *window = data;
  window->committed = window->pending;
  fullscreen_recompute();
  wayland_request_redraw();
}
static void closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle) {
  tracked_t *window = data;
  zwlr_foreign_toplevel_handle_v1_destroy(handle);
  *window = (tracked_t){0};
  fullscreen_recompute();
  wayland_request_redraw();
}
static void text_event(void *data,
                       struct zwlr_foreign_toplevel_handle_v1 *handle,
                       const char *value) {
  (void)data;
  (void)handle;
  (void)value;
}
static void parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                   struct zwlr_foreign_toplevel_handle_v1 *object) {
  (void)data;
  (void)handle;
  (void)object;
}
static const struct zwlr_foreign_toplevel_handle_v1_listener LISTENER = {
    .title = text_event,
    .app_id = text_event,
    .state = state,
    .output_enter = enter,
    .output_leave = leave,
    .done = done,
    .closed = closed,
    .parent = parent};
static void toplevel(void *data,
                     struct zwlr_foreign_toplevel_manager_v1 *object,
                     struct zwlr_foreign_toplevel_handle_v1 *handle) {
  (void)data;
  (void)object;
  for (size_t i = 0; i < MAX_TOPLEVELS; i++) {
    if (!tracked[i].handle) {
      tracked[i].handle = handle;
      zwlr_foreign_toplevel_handle_v1_add_listener(handle, &LISTENER,
                                                   &tracked[i]);
      return;
    }
  }
  zwlr_foreign_toplevel_handle_v1_destroy(handle);
}
static void finished(void *data,
                     struct zwlr_foreign_toplevel_manager_v1 *object) {
  (void)data;
  zwlr_foreign_toplevel_manager_v1_destroy(object);
  manager = NULL;
}
static const struct zwlr_foreign_toplevel_manager_v1_listener MANAGER_LISTENER =
    {.toplevel = toplevel, .finished = finished};
void fullscreen_init(struct zwlr_foreign_toplevel_manager_v1 *object) {
  manager = object;
  zwlr_foreign_toplevel_manager_v1_add_listener(object, &MANAGER_LISTENER,
                                                NULL);
}
void fullscreen_output_removed(uint32_t id) {
  struct wl_output *object = NULL;
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    if (outputs[i].name == id) {
      object = outputs[i].wl_output;
    }
  }
  if (!object) {
    return;
  }
  for (size_t i = 0; i < MAX_TOPLEVELS; i++) {
    for (size_t j = 0; j < MAX_OUTPUTS; j++) {
      if (tracked[i].pending.outputs[j] == object) {
        tracked[i].pending.outputs[j] = NULL;
      }
      if (tracked[i].committed.outputs[j] == object) {
        tracked[i].committed.outputs[j] = NULL;
      }
    }
  }
  fullscreen_recompute();
  wayland_request_redraw();
}
void fullscreen_cleanup(void) {
  for (size_t i = 0; i < MAX_TOPLEVELS; i++) {
    if (tracked[i].handle) {
      zwlr_foreign_toplevel_handle_v1_destroy(tracked[i].handle);
    }
  }
  memset(tracked, 0, sizeof(tracked));
  if (manager) {
    zwlr_foreign_toplevel_manager_v1_destroy(manager);
  }
  manager = NULL;
  output_events = false;
}
bool fs_detector_available(void) {
  return manager != NULL;
}
