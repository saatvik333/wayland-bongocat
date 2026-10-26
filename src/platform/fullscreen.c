#include "platform/fullscreen.h"

#include "core/bongocat.h"
#include "platform/niri.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#ifdef __GNUC__
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#endif
#include "wlr-foreign-toplevel-management-v1-client-protocol.h"
#ifdef __GNUC__
#  pragma GCC diagnostic pop
#endif

typedef struct {
  bool fullscreen;
  bool activated;
  struct wl_output *outputs[MAX_OUTPUTS];
} window_state_t;

typedef struct {
  struct wl_list link;
  struct zwlr_foreign_toplevel_handle_v1 *handle;
  window_state_t pending;
  window_state_t committed;
  uint64_t activation;  // Order of the latest activation; 0 if never
} toplevel_t;

typedef struct {
  struct wl_output *output;
  uint64_t activation;
} forgotten_t;

static struct zwlr_foreign_toplevel_manager_v1 *manager;
static struct wl_list toplevels;
static uint64_t activations;
// Windows activated at or before these orders no longer count.
static uint64_t forgotten_everywhere;
static forgotten_t forgotten[MAX_OUTPUTS];
// Compositors that never send output events place windows everywhere.
static bool output_events;
static bool changed;
// Outputs of windows that lost focus in the current event batch.
static struct wl_output *focus_lost[MAX_OUTPUTS];
static bool focus_lost_unknown;  // A window without output events lost focus

static bool on_output(const toplevel_t *window, struct wl_output *output) {
  if (!output_events) {
    return true;
  }
  for (size_t i = 0; output && i < MAX_OUTPUTS; i++) {
    if (window->committed.outputs[i] == output) {
      return true;
    }
  }
  return false;
}

static uint64_t forgotten_on(struct wl_output *output) {
  uint64_t order = forgotten_everywhere;
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    if (forgotten[i].output == output && forgotten[i].activation > order) {
      order = forgotten[i].activation;
    }
  }
  return order;
}

bool fullscreen_on(struct wl_output *output) {
  if (!manager) {
    return false;
  }
  uint64_t after = forgotten_on(output);
  const toplevel_t *last = NULL;
  toplevel_t *window;
  wl_list_for_each(window, &toplevels, link) {
    if (window->activation > after && on_output(window, output) &&
        (!last || window->activation > last->activation)) {
      last = window;
    }
  }
  return last && last->committed.fullscreen;
}

struct wl_output *fullscreen_focused_output(struct wl_output *current) {
  if (!manager) {
    return NULL;
  }
  const toplevel_t *focused = NULL;
  toplevel_t *window;
  wl_list_for_each(window, &toplevels, link) {
    if (window->committed.activated &&
        (!focused || window->activation > focused->activation)) {
      focused = window;
    }
  }
  struct wl_output *first = NULL;
  for (size_t i = 0; focused && i < MAX_OUTPUTS; i++) {
    struct wl_output *output = focused->committed.outputs[i];
    if (output && output == current) {
      return current;
    }
    first = first ? first : output;
  }
  return first;
}

void fullscreen_forget(struct wl_output *output) {
  forgotten_t *slot = NULL;
  for (size_t i = 0; output && i < MAX_OUTPUTS; i++) {
    if (forgotten[i].output == output) {
      slot = &forgotten[i];
      break;
    }
    if (!slot && !forgotten[i].output) {
      slot = &forgotten[i];
    }
  }
  if (slot) {
    *slot = (forgotten_t){.output = output, .activation = activations};
    changed = true;
  }
}

bool fullscreen_take_changed(void) {
  bool result = changed;
  changed = false;
  return result;
}

bool fullscreen_available(void) {
  return manager != NULL;
}

// =============================================================================
// PROTOCOL EVENTS
// =============================================================================

static void state(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                  struct wl_array *values) {
  (void)handle;
  toplevel_t *window = data;
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
                  struct wl_output *output) {
  (void)handle;
  toplevel_t *window = data;
  if (!output_events) {
    output_events = true;
    changed = true;
  }
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    if (window->pending.outputs[i] == output) {
      return;
    }
  }
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    if (!window->pending.outputs[i]) {
      window->pending.outputs[i] = output;
      return;
    }
  }
}

static void leave(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                  struct wl_output *output) {
  (void)handle;
  toplevel_t *window = data;
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    if (window->pending.outputs[i] == output) {
      window->pending.outputs[i] = NULL;
    }
  }
}

static bool any_activated(void) {
  toplevel_t *window;
  wl_list_for_each(window, &toplevels, link) {
    if (window->committed.activated) {
      return true;
    }
  }
  return false;
}

static bool same_state(const window_state_t *a, const window_state_t *b) {
  return a->fullscreen == b->fullscreen && a->activated == b->activated &&
         memcmp(a->outputs, b->outputs, sizeof(a->outputs)) == 0;
}

// Remembers the outputs of a window losing focus in the current batch.
static void note_focus_lost(const window_state_t *state) {
  if (!state->activated) {
    return;
  }
  focus_lost_unknown |= !output_events;
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    for (size_t j = 0; state->outputs[i] && j < MAX_OUTPUTS; j++) {
      if (!focus_lost[j] || focus_lost[j] == state->outputs[i]) {
        focus_lost[j] = state->outputs[i];
        break;
      }
    }
  }
}

static void done(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle) {
  (void)handle;
  toplevel_t *window = data;
  // Titles and app ids also end with done; only these fields matter.
  if (same_state(&window->pending, &window->committed)) {
    return;
  }
  if (window->pending.activated && !window->committed.activated) {
    window->activation = ++activations;
  }
  if (!window->pending.activated) {
    note_focus_lost(&window->committed);
  }
  window->committed = window->pending;
  changed = true;
}

static void closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle) {
  toplevel_t *window = data;
  note_focus_lost(&window->committed);
  zwlr_foreign_toplevel_handle_v1_destroy(handle);
  wl_list_remove(&window->link);
  free(window);
  changed = true;
}

void fullscreen_settle(void) {
  // Focus moves as a batch: a deactivation, then usually an activation. When
  // nothing is focused afterwards, an empty workspace took focus. niri reports
  // its output; otherwise assume the output focus left, the usual workspace
  // switch.
  if (manager && !any_activated() && !niri_focused_output()) {
    if (focus_lost_unknown) {
      forgotten_everywhere = activations;
    }
    for (size_t i = 0; i < MAX_OUTPUTS; i++) {
      if (focus_lost[i]) {
        fullscreen_forget(focus_lost[i]);
      }
    }
  }
  memset(focus_lost, 0, sizeof(focus_lost));
  focus_lost_unknown = false;
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
    .parent = parent,
};

static void toplevel(void *data,
                     struct zwlr_foreign_toplevel_manager_v1 *object,
                     struct zwlr_foreign_toplevel_handle_v1 *handle) {
  (void)data;
  (void)object;
  toplevel_t *window = calloc(1, sizeof(*window));
  if (!window) {
    zwlr_foreign_toplevel_handle_v1_destroy(handle);
    return;
  }
  window->handle = handle;
  wl_list_insert(&toplevels, &window->link);
  zwlr_foreign_toplevel_handle_v1_add_listener(handle, &LISTENER, window);
}

static void destroy_toplevels(void) {
  toplevel_t *window;
  toplevel_t *next;
  wl_list_for_each_safe(window, next, &toplevels, link) {
    zwlr_foreign_toplevel_handle_v1_destroy(window->handle);
    wl_list_remove(&window->link);
    free(window);
  }
}

static void finished(void *data,
                     struct zwlr_foreign_toplevel_manager_v1 *object) {
  (void)data;
  zwlr_foreign_toplevel_manager_v1_destroy(object);
  manager = NULL;
  destroy_toplevels();
  changed = true;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener MANAGER_LISTENER =
    {
        .toplevel = toplevel,
        .finished = finished,
};

void fullscreen_init(struct zwlr_foreign_toplevel_manager_v1 *object) {
  manager = object;
  wl_list_init(&toplevels);
  zwlr_foreign_toplevel_manager_v1_add_listener(object, &MANAGER_LISTENER,
                                                NULL);
}

void fullscreen_output_removed(struct wl_output *output) {
  if (!manager) {
    return;
  }
  toplevel_t *window;
  wl_list_for_each(window, &toplevels, link) {
    for (size_t i = 0; i < MAX_OUTPUTS; i++) {
      if (window->pending.outputs[i] == output) {
        window->pending.outputs[i] = NULL;
      }
      if (window->committed.outputs[i] == output) {
        window->committed.outputs[i] = NULL;
      }
    }
  }
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    if (forgotten[i].output == output) {
      forgotten[i] = (forgotten_t){0};
    }
    if (focus_lost[i] == output) {
      focus_lost[i] = NULL;
    }
  }
  changed = true;
}

void fullscreen_cleanup(void) {
  if (manager) {
    destroy_toplevels();
    zwlr_foreign_toplevel_manager_v1_destroy(manager);
  }
  manager = NULL;
  activations = 0;
  forgotten_everywhere = 0;
  memset(forgotten, 0, sizeof(forgotten));
  output_events = false;
  memset(focus_lost, 0, sizeof(focus_lost));
  focus_lost_unknown = false;
  changed = false;
}
