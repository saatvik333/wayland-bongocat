#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "platform/fullscreen.h"

#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#endif
#include "../protocols/wlr-foreign-toplevel-management-v1-client-protocol.h"
#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

#include "core/bongocat.h"
#include "platform/hyprland.h"
#include "platform/wayland.h"
#include "utils/error.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

// =============================================================================
// FULLSCREEN DETECTION MODULE
// =============================================================================

typedef struct {
  struct zwlr_foreign_toplevel_manager_v1 *manager;
  bool has_fullscreen_toplevel;
} fullscreen_detector_t;

// Foreign toplevel protocol event handlers
// Each toplevel tracks its fullscreen and activated state
typedef struct {
  bool is_fullscreen;
  bool is_activated;  // Track if this toplevel is the currently focused one
} toplevel_data_t;

typedef struct {
  struct zwlr_foreign_toplevel_handle_v1 *handle;
  struct wl_output *output;
  bool is_fullscreen;
  bool is_activated;
  toplevel_data_t *data;
} tracked_toplevel_t;

static fullscreen_detector_t fs_detector = {0};

static tracked_toplevel_t track_toplevels[MAX_TOPLEVELS] = {0};
static size_t track_toplevels_count = 0;

// Track whether the compositor has ever sent output_enter for any toplevel.
// When false, the compositor likely doesn't support per-toplevel output
// tracking (e.g. older KDE/KWin), and we should use the global fallback
// regardless of output_count.
static bool compositor_sends_output_events = false;

// =============================================================================
// FULLSCREEN DETECTION IMPLEMENTATION
// =============================================================================

static void fs_update_state(bool new_state) {
  if (new_state != fs_detector.has_fullscreen_toplevel) {
    fs_detector.has_fullscreen_toplevel = new_state;
    atomic_store(&fullscreen_detected, new_state);

    bongocat_log_info("Fullscreen state changed: %s",
                      new_state ? "detected" : "cleared");

    wayland_request_redraw();
  }
}

static void fs_recompute_state(void) {
  bool fullscreen = false;
  for (size_t i = 0; i < track_toplevels_count; i++) {
    bool relevant = fullscreen_toplevel_relevant(
        compositor_sends_output_events, track_toplevels[i].output == output,
        track_toplevels[i].is_activated);
    if (relevant && track_toplevels[i].is_fullscreen) {
      fullscreen = true;
      break;
    }
  }
  fs_update_state(fullscreen);
}

static bool update_fullscreen_state_toplevel(tracked_toplevel_t *tracked,
                                             bool is_fullscreen,
                                             bool is_activated) {
  tracked->is_fullscreen = is_fullscreen;
  tracked->is_activated = is_activated;

  /// @NOTE: tracked.output can always be NULL when no output.enter/output.leave
  /// event were triggered
  // Only trigger overlay update if this fullscreen window is on our output
  return tracked->output == output;
}

static bool hypr_fs_update_state(toplevel_data_t *toplevel_data) {
  window_info_t win;
  if (hypr_get_active_window(&win)) {
    bool found_output = false;
    struct wl_output *found_wl_output = NULL;
    for (size_t i = 0; i < MAX_OUTPUTS; i++) {
      if (outputs[i].hypr_id == win.monitor_id) {
        if (output == outputs[i].wl_output) {
          found_wl_output = output;
          found_output = true;
          break;
        }
      }
    }

    if (found_output) {
      toplevel_data->is_activated = true;
      toplevel_data->is_fullscreen = win.fullscreen;

      struct wl_output *current_wl_output = wayland_get_current_screen_output();
      if (current_wl_output && current_wl_output == found_wl_output) {
        fs_update_state(win.fullscreen);
      }
    }

    return found_output;
  }

  return false;
}

static void
fs_handle_toplevel_state(void *data,
                         struct zwlr_foreign_toplevel_handle_v1 *handle,
                         struct wl_array *state) {
  toplevel_data_t *toplevel_data = (toplevel_data_t *)data;
  if (!toplevel_data)
    return;

  bool is_fullscreen = false;
  bool is_activated = false;
  uint32_t *state_ptr;

  wl_array_for_each(state_ptr, state) {
    if (*state_ptr == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN) {
      is_fullscreen = true;
    }
    if (*state_ptr == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) {
      is_activated = true;
    }
  }

  /// @NOTE: tracked.output can be NULL when no output.enter/output.leave
  /// event were triggered
  bool output_found = false;
  bool handle_tracked = false;
  bool handle_has_output = false;
  for (size_t i = 0; i < track_toplevels_count; i++) {
    if (track_toplevels[i].handle == handle) {
      handle_tracked = true;
      if (track_toplevels[i].output != NULL) {
        handle_has_output = true;
      }
      output_found |= update_fullscreen_state_toplevel(
          &track_toplevels[i], is_fullscreen, is_activated);
    }
  }
  fs_recompute_state();

  // This toplevel is known to belong to a different output. Do not use
  // compositor-global fallbacks, otherwise fullscreen on monitor A can hide
  // overlay on monitor B.
  if (handle_tracked && handle_has_output && !output_found) {
    return;
  }

  // fallback: hyprland; when toplevel detection is not working
  if (!output_found) {
    output_found |= hypr_fs_update_state(toplevel_data);
  }

  // fallback: global fullscreen
  // Safe when single-output, or when the compositor never sends output_enter
  // events (e.g. KDE/KWin). In the latter case, per-output tracking is
  // impossible, so we use the global activated+fullscreen state as a best
  // effort. On multi-monitor setups without output events, fullscreen on
  // any monitor will hide the overlay on all monitors — acceptable trade-off
  // vs never hiding at all.
  if (!output_found && (output_count <= 1 || !compositor_sends_output_events)) {
    toplevel_data->is_fullscreen = is_fullscreen;
    toplevel_data->is_activated = is_activated;
    fs_recompute_state();
  }
}

static void
fs_handle_toplevel_closed(void *data,
                          struct zwlr_foreign_toplevel_handle_v1 *handle) {
  toplevel_data_t *toplevel_data = (toplevel_data_t *)data;
  if (!toplevel_data)
    return;

  free(toplevel_data);
  zwlr_foreign_toplevel_handle_v1_destroy(handle);

  // remove from track_toplevels if present
  for (size_t i = 0; i < track_toplevels_count; ++i) {
    if (track_toplevels[i].handle == handle) {
      track_toplevels[i].handle = NULL;
      track_toplevels[i].output = NULL;
      track_toplevels[i].is_fullscreen = false;
      track_toplevels[i].data = NULL;
      // compact array to keep contiguous
      for (size_t j = i; j + 1 < track_toplevels_count; ++j) {
        track_toplevels[j] = track_toplevels[j + 1];
      }
      track_toplevels_count--;
      break;
    }
  }
  fs_recompute_state();
}

// Minimal event handlers for unused events
static void fs_handle_title(void *data,
                            struct zwlr_foreign_toplevel_handle_v1 *handle,
                            const char *title) {
  (void)data;
  (void)handle;
  (void)title;
}

static void fs_handle_app_id(void *data,
                             struct zwlr_foreign_toplevel_handle_v1 *handle,
                             const char *app_id) {
  (void)data;
  (void)handle;
  (void)app_id;
}

static void
fs_handle_output_enter(void *data,
                       struct zwlr_foreign_toplevel_handle_v1 *handle,
                       struct wl_output *toplevel_output) {
  (void)data;

  compositor_sends_output_events = true;

  for (size_t i = 0; i < track_toplevels_count; i++) {
    if (track_toplevels[i].handle == handle) {
      track_toplevels[i].output = toplevel_output;
      fs_recompute_state();
      break;
    }
  }
}

static void
fs_handle_output_leave(void *data,
                       struct zwlr_foreign_toplevel_handle_v1 *handle,
                       struct wl_output *toplevel_output) {
  (void)data;

  for (size_t i = 0; i < track_toplevels_count; i++) {
    if (track_toplevels[i].handle == handle &&
        track_toplevels[i].output == toplevel_output) {
      track_toplevels[i].output = NULL;
      fs_recompute_state();
      break;
    }
  }
}

static void fs_handle_done(void *data,
                           struct zwlr_foreign_toplevel_handle_v1 *handle) {
  (void)data;
  (void)handle;
}

static void fs_handle_parent(void *data,
                             struct zwlr_foreign_toplevel_handle_v1 *handle,
                             struct zwlr_foreign_toplevel_handle_v1 *parent) {
  (void)data;
  (void)handle;
  (void)parent;
}

static const struct zwlr_foreign_toplevel_handle_v1_listener
    fs_toplevel_listener = {
        .title = fs_handle_title,
        .app_id = fs_handle_app_id,
        .output_enter = fs_handle_output_enter,
        .output_leave = fs_handle_output_leave,
        .state = fs_handle_toplevel_state,
        .done = fs_handle_done,
        .closed = fs_handle_toplevel_closed,
        .parent = fs_handle_parent,
};

static void
fs_handle_manager_toplevel(void *data,
                           struct zwlr_foreign_toplevel_manager_v1 *manager,
                           struct zwlr_foreign_toplevel_handle_v1 *toplevel) {
  (void)data;
  (void)manager;

  // Allocate per-toplevel data to track its fullscreen state
  toplevel_data_t *toplevel_data = malloc(sizeof(toplevel_data_t));
  if (!toplevel_data) {
    bongocat_log_error("Failed to allocate toplevel data");
    return;
  }

  // Check tracker capacity before registering listener
  if (track_toplevels_count >= MAX_TOPLEVELS) {
    bongocat_log_error("toplevel tracker is full, %zu max: %d",
                       track_toplevels_count, MAX_TOPLEVELS);
    free(toplevel_data);
    zwlr_foreign_toplevel_handle_v1_destroy(toplevel);
    return;
  }

  // Initialize: toplevel starts as not fullscreen and not activated
  toplevel_data->is_fullscreen = false;
  toplevel_data->is_activated = false;

  zwlr_foreign_toplevel_handle_v1_add_listener(toplevel, &fs_toplevel_listener,
                                               toplevel_data);

  bool already_tracked = false;
  for (size_t i = 0; i < track_toplevels_count; i++) {
    if (track_toplevels[i].handle == toplevel) {
      already_tracked = true;
      break;
    }
  }
  if (!already_tracked) {
    track_toplevels[track_toplevels_count].handle = toplevel;
    track_toplevels[track_toplevels_count].output = NULL;
    track_toplevels[track_toplevels_count].is_fullscreen = false;
    track_toplevels[track_toplevels_count].data = toplevel_data;
    track_toplevels_count++;
  }

  bongocat_log_debug("New toplevel registered for fullscreen monitoring");
}

static void
fs_handle_manager_finished(void *data,
                           struct zwlr_foreign_toplevel_manager_v1 *manager) {
  (void)data;
  bongocat_log_info("Foreign toplevel manager finished");
  zwlr_foreign_toplevel_manager_v1_destroy(manager);
  fs_detector.manager = NULL;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener
    fs_manager_listener = {
        .toplevel = fs_handle_manager_toplevel,
        .finished = fs_handle_manager_finished,
};

// =============================================================================
// PUBLIC API
// =============================================================================

void fullscreen_init(struct zwlr_foreign_toplevel_manager_v1 *manager) {
  if (!manager)
    return;

  fs_detector.manager = manager;
  zwlr_foreign_toplevel_manager_v1_add_listener(manager, &fs_manager_listener,
                                                NULL);
  bongocat_log_info("Foreign toplevel manager bound - using Wayland "
                    "protocol for fullscreen detection");
}

void fullscreen_cleanup(void) {
  for (size_t i = 0; i < track_toplevels_count; i++) {
    if (track_toplevels[i].handle)
      zwlr_foreign_toplevel_handle_v1_destroy(track_toplevels[i].handle);
    free(track_toplevels[i].data);
  }
  if (fs_detector.manager) {
    zwlr_foreign_toplevel_manager_v1_destroy(fs_detector.manager);
    fs_detector.manager = NULL;
  }

  memset(&fs_detector, 0, sizeof(fs_detector));
  memset(track_toplevels, 0, sizeof(track_toplevels));
  track_toplevels_count = 0;
  compositor_sends_output_events = false;
}

bool fs_detector_available(void) {
  return fs_detector.manager != NULL;
}
