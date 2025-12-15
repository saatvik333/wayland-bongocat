#define _POSIX_C_SOURCE 200809L
#include "platform/wayland.h"

#include "../protocols/wlr-foreign-toplevel-management-v1-client-protocol.h"
#include "../protocols/xdg-output-unstable-v1-client-protocol.h"
#include "graphics/animation.h"

#include <poll.h>
#include <stdatomic.h>
#include <sys/time.h>

// =============================================================================
// GLOBAL STATE AND CONFIGURATION
// =============================================================================

// Wayland globals
atomic_bool configured = false;
atomic_bool fullscreen_detected = false;
struct wl_display *display;
struct wl_compositor *compositor;
struct wl_shm *shm;
struct zwlr_layer_shell_v1 *layer_shell;
struct xdg_wm_base *xdg_wm_base;
struct wl_output *output;
struct wl_surface *surface;
struct wl_buffer *buffer;
struct zwlr_layer_surface_v1 *layer_surface;
uint8_t *pixels;

static config_t *current_config;

// =============================================================================
// SCREEN DIMENSION MANAGEMENT
// =============================================================================

typedef struct {
  int screen_width;
  int screen_height;
  int transform;
  int raw_width;
  int raw_height;
  bool mode_received;
  bool geometry_received;
  struct wl_output *wl_output;
} screen_info_t;

static screen_info_t screen_infos[MAX_OUTPUTS] = {0};
static output_ref_t outputs[MAX_OUTPUTS];
static size_t output_count = 0;
static struct zxdg_output_manager_v1 *xdg_output_manager = NULL;
static screen_info_t *current_screen_info = NULL;

// Output reconnection handling
static struct wl_registry *global_registry = NULL;
static uint32_t bound_output_name = 0;   // Registry name of our bound output
static atomic_bool output_lost = false;  // Set when our output disconnects
static bool using_named_output =
    false;  // True if user specified an output name
static char *bound_screen_name = NULL;

BONGOCAT_NODISCARD static struct wl_output *wayland_find_new_output(void) {
  struct wl_output *matching_wl_output = NULL;
  if (current_config->output_name) {
    for (size_t i = 0; i < output_count; ++i) {
      if (outputs[i].name_received &&
          strcmp(outputs[i].name_str, current_config->output_name) == 0) {
        return outputs[i].wl_output;
      }
    }
  }
  return NULL;
}

BONGOCAT_NODISCARD static int wayland_get_new_screen_width(void) {
  struct wl_output *matching_wl_output = wayland_find_new_output();
  for (size_t i = 0; i < output_count; ++i) {
    if (screen_infos[i].wl_output == matching_wl_output) {
      return screen_infos[i].screen_width;
    }
  }

  return 0;
}

static void wayland_update_current_screen_info(void) {
  bool output_found = false;
  if (output) {
    wl_display_roundtrip(display);

    for (size_t i = 0; i < MAX_OUTPUTS; i++) {
      if (outputs[i].wl_output == output) {
        bongocat_log_info("Detected screen name: %s", outputs[i].name_str);
        bound_screen_name = outputs[i].name_str;
      }
      if (screen_infos[i].wl_output == output) {
        if (screen_infos[i].screen_width > 0) {
          bongocat_log_info("Detected screen width: %d",
                            screen_infos[i].screen_width);
          current_screen_info = &screen_infos[i];
          current_config->screen_width = screen_infos[i].screen_width;
          output_found = true;
        }
      }
    }
  }

  if (!output_found) {
    bongocat_log_warning("No output found, using default screen width: %d",
                         DEFAULT_SCREEN_WIDTH);
    current_config->screen_width = DEFAULT_SCREEN_WIDTH;
    current_screen_info = NULL;
  }
}

// =============================================================================
// ZXDG LISTENER IMPLEMENTATION
// =============================================================================

// Forward declarations for reconnection handling
static bongocat_error_t wayland_setup_surface(void);

static void handle_xdg_output_name(void *data,
                                   struct zxdg_output_v1 *xdg_output
                                   __attribute__((unused)),
                                   const char *name) {
  // Defensive null check
  if (!data || !name) {
    return;
  }

  output_ref_t *oref = data;
  snprintf(oref->name_str, sizeof(oref->name_str), "%s", name);
  oref->name_received = true;
  bongocat_log_debug("xdg-output name received: %s", name);

  // Check if this is the output we're waiting for (reconnection case)
  if (!atomic_load(&output_lost) || !current_config) {
    return;
  }

  bool should_reconnect = false;

  // Case 1: User specified an output name - match exactly
  if (using_named_output && current_config->output_name) {
    should_reconnect = (strcmp(name, current_config->output_name) == 0);
  }
  // Case 2: Using fallback (first output) - reconnect to any output
  else if (!using_named_output) {
    should_reconnect = true;
    bongocat_log_debug("Using fallback output, accepting '%s'", name);
  }

  if (should_reconnect) {
    bongocat_log_info("Target output '%s' reconnected!", name);

    // Clean up old surface if it exists
    if (layer_surface) {
      zwlr_layer_surface_v1_destroy(layer_surface);
      layer_surface = NULL;
    }
    if (surface) {
      wl_surface_destroy(surface);
      surface = NULL;
    }

    // Set new output
    output = oref->wl_output;
    bound_output_name = oref->name;
    atomic_store(&output_lost, false);
    bound_screen_name = oref->name_str;

    // Recreate surface on new output
    // Note: wayland_setup_surface already commits, triggering a configure
    // event. The layer_surface_configure callback will ack and call draw_bar()
    // to render.
    if (wayland_setup_surface() == BONGOCAT_SUCCESS) {
      // Wait for configure event to be processed
      wl_display_roundtrip(display);
      wayland_update_current_screen_info();
      bongocat_log_info("Surface recreated, configure event processed");
    } else {
      bongocat_log_error("Failed to recreate surface on reconnected output");
    }
  }
}

static void handle_xdg_output_logical_position(
    void *data, struct zxdg_output_v1 *xdg_output, int32_t x, int32_t y) {
  // Defensive null check
  if (!data) {
    return;
  }

  output_ref_t *oref = data;

  oref->x = x;
  oref->y = y;

  bongocat_log_debug("xdg-output logical position received: %d,%d", x, y);
}
static void handle_xdg_output_logical_size(void *data,
                                           struct zxdg_output_v1 *xdg_output,
                                           int32_t width, int32_t height) {
  // Defensive null check
  if (!data) {
    return;
  }

  output_ref_t *oref = data;

  oref->width = width;
  oref->height = height;

  bongocat_log_debug("xdg-output logical size received: %dx%d", width, height);
}
static void handle_xdg_output_done(void *data,
                                   struct zxdg_output_v1 *xdg_output) {}

static void handle_xdg_output_description(void *data,
                                          struct zxdg_output_v1 *xdg_output,
                                          const char *description) {
  (void)data;
  (void)xdg_output;
  (void)description;
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_position = handle_xdg_output_logical_position,
    .logical_size = handle_xdg_output_logical_size,
    .done = handle_xdg_output_done,
    .name = handle_xdg_output_name,
    .description = handle_xdg_output_description};

// =============================================================================
// FULLSCREEN DETECTION MODULE
// =============================================================================

typedef struct {
  struct zwlr_foreign_toplevel_manager_v1 *manager;
  bool has_fullscreen_toplevel;
} fullscreen_detector_t;

typedef struct {
  struct zwlr_foreign_toplevel_handle_v1 *handle;
  struct wl_output *output;
  bool is_fullscreen;
  bool is_activated;
} tracked_toplevel_t;

static fullscreen_detector_t fs_detector = {0};

static tracked_toplevel_t track_toplevels[MAX_TOPLEVELS] = {0};
static size_t track_toplevels_count = 0;

typedef struct {
  int monitor_id;  // monitor number in Hyprland
  int x, y;
  int width, height;
  bool fullscreen;
} window_info_t;

// =============================================================================
// FULLSCREEN DETECTION IMPLEMENTATION
// =============================================================================

static void fs_update_state(bool new_state) {
  if (new_state != fs_detector.has_fullscreen_toplevel) {
    fs_detector.has_fullscreen_toplevel = new_state;
    atomic_store(&fullscreen_detected, new_state);

    bongocat_log_info("Fullscreen state changed: %s",
                      new_state ? "detected" : "cleared");

    if (atomic_load(&configured)) {
      draw_bar();
    }
  }
}

static void hypr_update_outputs_with_monitor_ids(void) {
  FILE *fp = popen("hyprctl monitors 2>/dev/null", "r");
  if (!fp)
    return;

  char line[512];
  while (fgets(line, sizeof(line), fp)) {
    int id = -1;
    char name[256];
    int result = sscanf(line, "Monitor %d \"%255[^\"]\"", &id, name);
    if (result < 2) {
      result = sscanf(line, "Monitor %255s (ID %d)", name, &id);
    }
    if (result == 2) {
      for (size_t i = 0; i < output_count; i++) {
        // match by xdg-output name
        if (outputs[i].name_received &&
            strcmp(outputs[i].name_str, name) == 0) {
          outputs[i].hypr_id = id;
          bongocat_log_debug("Mapped xdg-output '%s' to Hyprland ID %d\n", name,
                             id);
          break;
        }
      }
    }
  }

  pclose(fp);
}

static bool hypr_get_active_window(window_info_t *win) {
  FILE *fp = popen("hyprctl activewindow 2>/dev/null", "r");
  if (!fp)
    return false;

  char line[512];
  bool has_window = false;
  win->monitor_id = -1;
  win->fullscreen = false;

  while (fgets(line, sizeof(line), fp)) {
    // monitor: 0
    if (strstr(line, "monitor:")) {
      sscanf(line, "%*[\t ]monitor: %d", &win->monitor_id);
      has_window = true;
    }
    // fullscreen: 0/1/2
    if (strstr(line, "fullscreen:")) {
      int val;
      if (sscanf(line, "%*[\t ]fullscreen: %d", &val) == 1) {
        win->fullscreen = (val != 0);
      }
    }
    // at: X,Y
    if (strstr(line, "at:")) {
      if (sscanf(line, "%*[\t ]at: [%d, %d]", &win->x, &win->y) < 2) {
        sscanf(line, "%*[\t ]at: %d,%d", &win->x, &win->y);
      }
    }
    // size: W,H
    if (strstr(line, "size:")) {
      if (sscanf(line, "%*[\t ]size: [%d, %d]", &win->width, &win->height) <
          2) {
        sscanf(line, "%*[\t ]size: %d,%d", &win->width, &win->height);
      }
    }
  }

  pclose(fp);
  return has_window;
}

static bool fs_check_compositor_fallback(void) {
  bongocat_log_debug("Using compositor-specific fullscreen detection");

  // Try Hyprland first
  window_info_t win;
  if (hypr_get_active_window(&win)) {
    return win.fullscreen;
  }

  // Try Sway as fallback
  FILE *fp = popen("swaymsg -t get_tree 2>/dev/null", "r");
  if (fp) {
    char sway_buffer[4096];
    bool is_fullscreen = false;

    while (fgets(sway_buffer, sizeof(sway_buffer), fp)) {
      if (strstr(sway_buffer, "\"fullscreen_mode\":1")) {
        is_fullscreen = true;
        bongocat_log_debug("Fullscreen detected in Sway");
        break;
      }
    }
    pclose(fp);
    return is_fullscreen;
  }

  bongocat_log_debug("No supported compositor found for fullscreen detection");
  return false;
}

// Foreign toplevel protocol event handlers
// Each toplevel tracks its fullscreen and activated state
typedef struct {
  bool is_fullscreen;
  bool is_activated;  // Track if this toplevel is the currently focused one
} toplevel_data_t;

// Track the currently active toplevel's fullscreen state
static bool active_toplevel_fullscreen = false;

static bool fs_check_status(void) {
  if (fs_detector.manager) {
    return fs_detector.has_fullscreen_toplevel;
  }
  return fs_check_compositor_fallback();
}

static bool update_fullscreen_state_toplevel(tracked_toplevel_t *tracked,
                                             bool is_fullscreen,
                                             bool is_activated) {
  bool state_changed = tracked->is_fullscreen != is_fullscreen ||
                       tracked->is_activated != is_activated;
  tracked->is_fullscreen = is_fullscreen;
  tracked->is_activated = is_activated;

  /// @NOTE: tracked.output can always be NULL when no output.enter/output.leave
  /// event were triggert
  // Only trigger overlay update if this fullscreen window is on our output
  if (tracked->output == output && state_changed) {
    fs_update_state(is_fullscreen);
    return true;
  }

  return false;
}
static bool hypr_fs_update_state(toplevel_data_t *toplevel_data) {
  window_info_t win;
  if (hypr_get_active_window(&win)) {
    bool found_output = false;
    for (size_t i = 0; i < output_count; i++) {
      if (outputs[i].hypr_id == win.monitor_id) {
        if (output == outputs[i].wl_output) {
          found_output = true;
          break;
        }
      }
    }

    if (found_output) {
      toplevel_data->is_activated = true;
      toplevel_data->is_fullscreen = win.fullscreen;

      active_toplevel_fullscreen = win.fullscreen;
      fs_update_state(win.fullscreen);
    } else {
      // active window is not on the same screen as bongocat
      toplevel_data->is_activated = false;
      toplevel_data->is_fullscreen = false;
      active_toplevel_fullscreen = false;
      fs_update_state(false);
    }
    return true;
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
  /// event were triggerd
  bool output_found = false;
  for (size_t i = 0; i < track_toplevels_count; i++) {
    if (track_toplevels[i].handle == handle) {
      output_found |= update_fullscreen_state_toplevel(
          &track_toplevels[i], is_fullscreen, is_activated);
    }
  }

  // fallback: hyprland; when toplevel detection is not working
  if (!output_found) {
    output_found |= hypr_fs_update_state(toplevel_data);
  }

  // fallback: global fullscreen
  if (!output_found) {
    bool was_activated = toplevel_data->is_activated;
    bool was_fullscreen = toplevel_data->is_fullscreen;

    toplevel_data->is_fullscreen = is_fullscreen;
    toplevel_data->is_activated = is_activated;

    // Case 1: Window becomes active - update state based on its fullscreen
    // status
    if (is_activated) {
      active_toplevel_fullscreen = is_fullscreen;
      fs_update_state(is_fullscreen);
    }
    // Case 2: Previously active fullscreen window loses activation
    // (e.g., switching to empty workspace) - show bongocat
    else if (was_activated && was_fullscreen && !is_activated) {
      active_toplevel_fullscreen = false;
      fs_update_state(false);
    }
  }
}

static void
fs_handle_toplevel_closed(void *data,
                          struct zwlr_foreign_toplevel_handle_v1 *handle) {
  toplevel_data_t *toplevel_data = (toplevel_data_t *)data;
  if (!toplevel_data)
    return;

  if (toplevel_data) {
    // If the closed toplevel was the active fullscreen one, clear state
    if (toplevel_data->is_activated && toplevel_data->is_fullscreen) {
      active_toplevel_fullscreen = false;
      fs_update_state(false);
    }
    free(toplevel_data);
  }
  zwlr_foreign_toplevel_handle_v1_destroy(handle);

  // remove from track_toplevels if present
  for (size_t i = 0; i < track_toplevels_count; ++i) {
    if (track_toplevels[i].handle == handle) {
      track_toplevels[i].handle = NULL;
      track_toplevels[i].output = NULL;
      track_toplevels[i].is_fullscreen = false;
      // compact array to keep contiguous
      for (size_t j = i; j + 1 < track_toplevels_count; ++j) {
        track_toplevels[j] = track_toplevels[j + 1];
      }
      track_toplevels_count--;
      break;
    }
  }
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
  (void)handle;
  (void)toplevel_output;

  for (size_t i = 0; i < track_toplevels_count; i++) {
    if (track_toplevels[i].handle == handle) {
      track_toplevels[i].output = output;
      if (track_toplevels[i].is_fullscreen) {
        if (track_toplevels[i].output == output) {
          fs_update_state(true);
        }
      }
      break;
    }
  }
}

static void
fs_handle_output_leave(void *data,
                       struct zwlr_foreign_toplevel_handle_v1 *handle,
                       struct wl_output *toplevel_output) {
  (void)data;
  (void)toplevel_output;

  for (size_t i = 0; i < track_toplevels_count; i++) {
    if (track_toplevels[i].handle == handle &&
        track_toplevels[i].output == output) {
      if (track_toplevels[i].is_fullscreen &&
          track_toplevels[i].output == output) {
        fs_update_state(false);
      }
      track_toplevels[i].output = NULL;
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

  // Initialize: toplevel starts as not fullscreen and not activated
  // State events will update these values
  toplevel_data->is_fullscreen = false;
  toplevel_data->is_activated = false;

  zwlr_foreign_toplevel_handle_v1_add_listener(toplevel, &fs_toplevel_listener,
                                               toplevel_data);

  if (track_toplevels_count < MAX_TOPLEVELS) {
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
      track_toplevels_count++;
    }
  } else {
    bongocat_log_error("toplevel tracker is full, %zu max: %d",
                       track_toplevels_count, MAX_TOPLEVELS);
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
// SCREEN DIMENSION MANAGEMENT
// =============================================================================

static void screen_calculate_dimensions(screen_info_t *screen_info) {
  if (!screen_info) {
    return;
  }

  if (!screen_info->mode_received || !screen_info->geometry_received ||
      screen_info->screen_width > 0) {
    return;
  }

  bool is_rotated = (screen_info->transform == WL_OUTPUT_TRANSFORM_90 ||
                     screen_info->transform == WL_OUTPUT_TRANSFORM_270 ||
                     screen_info->transform == WL_OUTPUT_TRANSFORM_FLIPPED_90 ||
                     screen_info->transform == WL_OUTPUT_TRANSFORM_FLIPPED_270);

  if (is_rotated) {
    screen_info->screen_width = screen_info->raw_height;
    screen_info->screen_height = screen_info->raw_width;
    bongocat_log_info("Detected rotated screen: %dx%d (transform: %d)",
                      screen_info->raw_height, screen_info->raw_width,
                      screen_info->transform);
  } else {
    screen_info->screen_width = screen_info->raw_width;
    screen_info->screen_height = screen_info->raw_height;
    bongocat_log_info("Detected screen: %dx%d (transform: %d)",
                      screen_info->raw_width, screen_info->raw_height,
                      screen_info->transform);
  }
}

// =============================================================================
// BUFFER AND DRAWING MANAGEMENT
// =============================================================================

int create_shm(int size) {
  char name[] = "/bar-shm-XXXXXX";
  int fd;

  for (int i = 0; i < 100; i++) {
    for (int j = 0; j < 6; j++) {
      name[9 + j] = 'A' + (rand() % 26);
    }
    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
      shm_unlink(name);
      break;
    }
  }

  if (fd < 0 || ftruncate(fd, size) < 0) {
    perror("shm");
    exit(1);
  }

  return fd;
}

void draw_bar(void) {
  if (!atomic_load(&configured)) {
    bongocat_log_debug("Surface not configured yet, skipping draw");
    return;
  }

  // Critical null checks - prevent crash during buffer recreation
  if (!current_config || !pixels) {
    bongocat_log_debug("Config or pixels not ready, skipping draw");
    return;
  }

  // Skip fullscreen hiding when layer is LAYER_OVERLAY (always visible)
  bool is_overlay_layer = current_config->layer == LAYER_OVERLAY;
  bool is_fullscreen = !is_overlay_layer && atomic_load(&fullscreen_detected);
  int effective_opacity = is_fullscreen ? 0 : current_config->overlay_opacity;

  // Clear buffer with transparency - OPTIMIZED
  // Use memset for RGB (zeros) then set only alpha bytes
  int buffer_size =
      current_config->screen_width * current_config->bar_height * 4;
  memset(pixels, 0, buffer_size);

  // Set alpha channel only (every 4th byte starting at offset 3)
  if (effective_opacity > 0) {
    for (int i = 3; i < buffer_size; i += 4) {
      pixels[i] = effective_opacity;
    }
  }

  // Draw cat if visible
  if (!is_fullscreen) {
    pthread_mutex_lock(&anim_lock);
    int cat_height = current_config->cat_height;
    int cat_width = (cat_height * CAT_IMAGE_WIDTH) / CAT_IMAGE_HEIGHT;
    int cat_y = (current_config->bar_height - cat_height) / 2 +
                current_config->cat_y_offset;

    int cat_x = 0;
    switch (current_config->cat_align) {
    case ALIGN_CENTER:
      cat_x = (current_config->screen_width - cat_width) / 2 +
              current_config->cat_x_offset;
      break;
    case ALIGN_LEFT:
      cat_x = current_config->cat_x_offset;
      break;
    case ALIGN_RIGHT:
      cat_x = current_config->screen_width - cat_width -
              current_config->cat_x_offset;
      break;
    }

    blit_image_scaled(pixels, current_config->screen_width,
                      current_config->bar_height, anim_imgs[anim_index],
                      anim_width[anim_index], anim_height[anim_index], cat_x,
                      cat_y, cat_width, cat_height);
    pthread_mutex_unlock(&anim_lock);
  } else {
    bongocat_log_debug("Cat hidden due to fullscreen detection");
  }

  wl_surface_attach(surface, buffer, 0, 0);
  wl_surface_damage_buffer(surface, 0, 0, current_config->screen_width,
                           current_config->bar_height);
  wl_surface_commit(surface);
  wl_display_flush(display);
}

// =============================================================================
// WAYLAND EVENT HANDLERS
// =============================================================================

static void layer_surface_configure(void *data __attribute__((unused)),
                                    struct zwlr_layer_surface_v1 *ls,
                                    uint32_t serial, uint32_t w, uint32_t h) {
  bongocat_log_debug("Layer surface configured: %dx%d", w, h);
  zwlr_layer_surface_v1_ack_configure(ls, serial);
  atomic_store(&configured, true);
  draw_bar();
}

// Handle compositor-requested surface closure
static void layer_surface_closed(void *data __attribute__((unused)),
                                 struct zwlr_layer_surface_v1 *ls
                                 __attribute__((unused))) {
  bongocat_log_info("Layer surface closed by compositor");
  atomic_store(&configured, false);
}

static struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

static void xdg_wm_base_ping(void *data __attribute__((unused)),
                             struct xdg_wm_base *wm_base, uint32_t serial) {
  xdg_wm_base_pong(wm_base, serial);
}

static struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

static void output_geometry(void *data __attribute__((unused)),
                            struct wl_output *wl_output,
                            int32_t x __attribute__((unused)),
                            int32_t y __attribute__((unused)),
                            int32_t physical_width __attribute__((unused)),
                            int32_t physical_height __attribute__((unused)),
                            int32_t subpixel __attribute__((unused)),
                            const char *make __attribute__((unused)),
                            const char *model __attribute__((unused)),
                            int32_t transform) {
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    if (screen_infos[i].wl_output == wl_output) {
      screen_infos[i].transform = transform;
      screen_infos[i].geometry_received = true;
      bongocat_log_debug("Output transform: %d", transform);
      screen_calculate_dimensions(&screen_infos[i]);
      break;
    }
  }
}

static void output_mode(void *data __attribute__((unused)),
                        struct wl_output *wl_output, uint32_t flags,
                        int32_t width, int32_t height,
                        int32_t refresh __attribute__((unused))) {
  if (flags & WL_OUTPUT_MODE_CURRENT) {
    for (size_t i = 0; i < MAX_OUTPUTS; i++) {
      if (screen_infos[i].wl_output == wl_output) {
        screen_infos[i].raw_width = width;
        screen_infos[i].raw_height = height;
        screen_infos[i].mode_received = true;
        bongocat_log_debug("Received raw screen mode: %dx%d", width, height);
        screen_calculate_dimensions(&screen_infos[i]);
        break;
      }
    }
  }
}

static void output_done(void *data __attribute__((unused)),
                        struct wl_output *wl_output) {
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    if (screen_infos[i].wl_output == wl_output) {
      screen_calculate_dimensions(&screen_infos[i]);
      bongocat_log_debug("Output configuration complete");
    }
  }
}

static void output_scale(void *data __attribute__((unused)),
                         struct wl_output *wl_output __attribute__((unused)),
                         int32_t factor __attribute__((unused))) {
  // Scale not needed for our use case
}

static struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
};

// =============================================================================
// WAYLAND PROTOCOL REGISTRY
// =============================================================================

static void registry_global(void *data __attribute__((unused)),
                            struct wl_registry *reg, uint32_t name,
                            const char *iface,
                            uint32_t ver __attribute__((unused))) {
  if (strcmp(iface, wl_compositor_interface.name) == 0) {
    compositor = (struct wl_compositor *)wl_registry_bind(
        reg, name, &wl_compositor_interface, 4);
  } else if (strcmp(iface, wl_shm_interface.name) == 0) {
    shm = (struct wl_shm *)wl_registry_bind(reg, name, &wl_shm_interface, 1);
  } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
    layer_shell = (struct zwlr_layer_shell_v1 *)wl_registry_bind(
        reg, name, &zwlr_layer_shell_v1_interface, 1);
  } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
    xdg_wm_base = (struct xdg_wm_base *)wl_registry_bind(
        reg, name, &xdg_wm_base_interface, 1);
    if (xdg_wm_base) {
      xdg_wm_base_add_listener(xdg_wm_base, &xdg_wm_base_listener, NULL);
    }
  } else if (strcmp(iface, zxdg_output_manager_v1_interface.name) == 0) {
    xdg_output_manager =
        wl_registry_bind(reg, name, &zxdg_output_manager_v1_interface, 3);
  } else if (strcmp(iface, wl_output_interface.name) == 0) {
    if (output_count < MAX_OUTPUTS) {
      outputs[output_count].name = name;
      outputs[output_count].wl_output =
          wl_registry_bind(reg, name, &wl_output_interface, 2);
      screen_infos[output_count].wl_output = outputs[output_count].wl_output;
      wl_output_add_listener(outputs[output_count].wl_output, &output_listener,
                             NULL);

      // If we lost our output, get xdg_output to check if this is the one
      // reconnecting
      if (atomic_load(&output_lost) && xdg_output_manager) {
        outputs[output_count].xdg_output =
            zxdg_output_manager_v1_get_xdg_output(
                xdg_output_manager, outputs[output_count].wl_output);
        outputs[output_count].name_received = false;
        zxdg_output_v1_add_listener(outputs[output_count].xdg_output,
                                    &xdg_output_listener,
                                    &outputs[output_count]);
        bongocat_log_debug(
            "New output appeared while output_lost, checking name...");
      }

      output_count++;
    }
  } else if (strcmp(iface, zwlr_foreign_toplevel_manager_v1_interface.name) ==
             0) {
    fs_detector.manager =
        (struct zwlr_foreign_toplevel_manager_v1 *)wl_registry_bind(
            reg, name, &zwlr_foreign_toplevel_manager_v1_interface, 3);
    if (fs_detector.manager) {
      zwlr_foreign_toplevel_manager_v1_add_listener(fs_detector.manager,
                                                    &fs_manager_listener, NULL);
      bongocat_log_info("Foreign toplevel manager bound - using Wayland "
                        "protocol for fullscreen detection");
    }
  }
}

static void registry_remove(void *data __attribute__((unused)),
                            struct wl_registry *registry
                            __attribute__((unused)),
                            uint32_t name) {
  // Check if the removed global is our bound output
  if (name == bound_output_name && bound_output_name != 0) {
    bongocat_log_warning("Bound output disconnected (registry name %u)", name);
    atomic_store(&output_lost, true);
    atomic_store(&configured, false);

    // Clean up the old output reference
    output = NULL;

    // Remove from outputs array
    for (size_t i = 0; i < output_count; ++i) {
      if (outputs[i].name == name) {
        if (outputs[i].xdg_output) {
          zxdg_output_v1_destroy(outputs[i].xdg_output);
          outputs[i].xdg_output = NULL;
        }
        if (outputs[i].wl_output) {
          wl_output_destroy(outputs[i].wl_output);
          outputs[i].wl_output = NULL;
          screen_infos[i].wl_output = NULL;
        }
        // Shift remaining outputs
        for (size_t j = i; j < output_count - 1; ++j) {
          outputs[j] = outputs[j + 1];
        }
        for (size_t j = i; j < output_count - 1; ++j) {
          screen_infos[j] = screen_infos[j + 1];
        }
        // Zero out the now-unused slot
        memset(&outputs[output_count - 1], 0, sizeof(output_ref_t));
        memset(&screen_infos[output_count - 1], 0, sizeof(screen_info_t));
        output_count--;
        break;
      }
    }
  }
}

static struct wl_registry_listener reg_listener = {
    .global = registry_global, .global_remove = registry_remove};

// =============================================================================
// MAIN WAYLAND INTERFACE IMPLEMENTATION
// =============================================================================

static void wayland_update_output(void) {
  output = NULL;
  bound_output_name = 0;
  using_named_output = false;
  current_screen_info = NULL;
  bound_screen_name = NULL;

  if (current_config->output_name) {
    for (size_t i = 0; i < output_count; ++i) {
      if (outputs[i].name_received &&
          strcmp(outputs[i].name_str, current_config->output_name) == 0) {
        output = outputs[i].wl_output;
        bound_output_name =
            outputs[i].name;  // Store registry name for tracking
        bound_screen_name = outputs[i].name_str;
        using_named_output = true;  // User specified this output
        current_screen_info = &screen_infos[i];
        bongocat_log_info("Matched output: %s (registry name %u, %s)",
                          outputs[i].name_str, bound_output_name,
                          bound_screen_name);
        break;
      }
    }

    if (!output) {
      bongocat_log_error(
          "Could not find output named '%s', defaulting to first output",
          current_config->output_name);
    }
  }

  // Fallback
  if (!output && output_count > 0) {
    output = outputs[0].wl_output;
    bound_output_name = outputs[0].name;
    bound_screen_name = outputs[0].name_str;
    for (size_t i = 0; i < MAX_OUTPUTS; i++) {
      if (screen_infos[i].wl_output == output) {
        current_screen_info = &screen_infos[i];
      }
    }
    using_named_output = false;  // Using fallback, not a named output
    bongocat_log_warning("Falling back to first output (registry name %u, %s)",
                         bound_output_name, bound_screen_name);
  }
}

static bongocat_error_t wayland_setup_protocols(void) {
  global_registry = wl_display_get_registry(display);
  if (!global_registry) {
    bongocat_log_error("Failed to get Wayland registry");
    return BONGOCAT_ERROR_WAYLAND;
  }

  wl_registry_add_listener(global_registry, &reg_listener, NULL);
  wl_display_roundtrip(display);

  if (xdg_output_manager) {
    for (size_t i = 0; i < output_count; ++i) {
      outputs[i].xdg_output = zxdg_output_manager_v1_get_xdg_output(
          xdg_output_manager, outputs[i].wl_output);
      outputs[i].x = 0;
      outputs[i].y = 0;
      outputs[i].width = 0;
      outputs[i].height = 0;
      outputs[i].hypr_id = -1;
      zxdg_output_v1_add_listener(outputs[i].xdg_output, &xdg_output_listener,
                                  &outputs[i]);
    }

    // Wait for all xdg_output events
    wl_display_roundtrip(display);

    hypr_update_outputs_with_monitor_ids();
  }

  wayland_update_output();

  if (!compositor || !shm || !layer_shell) {
    bongocat_log_error("Missing required Wayland protocols");
    wl_registry_destroy(global_registry);
    global_registry = NULL;
    return BONGOCAT_ERROR_WAYLAND;
  }

  // Configure screen dimensions
  wayland_update_current_screen_info();

  // Keep registry alive for output reconnection handling
  return BONGOCAT_SUCCESS;
}

// Helper to handle output reconnection
static void wayland_handle_output_reconnect(struct wl_output *new_output,
                                            uint32_t registry_name,
                                            const char *output_name) {
  bongocat_log_info("Output '%s' reconnected (registry name %u)", output_name,
                    registry_name);

  // Clean up old surface if it exists
  if (layer_surface) {
    zwlr_layer_surface_v1_destroy(layer_surface);
    layer_surface = NULL;
  }
  if (surface) {
    wl_surface_destroy(surface);
    surface = NULL;
  }

  // Set new output
  output = new_output;
  bound_output_name = registry_name;
  atomic_store(&output_lost, false);
  bound_screen_name = NULL;

  // Recreate surface on new output
  if (wayland_setup_surface() == BONGOCAT_SUCCESS) {
    bongocat_log_info("Surface recreated on reconnected output");
    wl_display_roundtrip(display);
    wayland_update_current_screen_info();
  } else {
    bongocat_log_error("Failed to recreate surface on reconnected output");
  }
}

static bongocat_error_t wayland_setup_surface(void) {
  surface = wl_compositor_create_surface(compositor);
  if (!surface) {
    bongocat_log_error("Failed to create surface");
    return BONGOCAT_ERROR_WAYLAND;
  }

  layer_surface = zwlr_layer_shell_v1_get_layer_surface(
      layer_shell, surface, output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
      "bongocat-overlay");

  if (!layer_surface) {
    bongocat_log_error("Failed to create layer surface");
    return BONGOCAT_ERROR_WAYLAND;
  }

  // Configure layer surface
  uint32_t anchor =
      ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
  if (current_config->overlay_position == POSITION_TOP) {
    anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
  } else {
    anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
  }

  zwlr_layer_surface_v1_set_anchor(layer_surface, anchor);
  zwlr_layer_surface_v1_set_size(layer_surface, 0, current_config->bar_height);
  zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, -1);
  zwlr_layer_surface_v1_set_keyboard_interactivity(
      layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
  zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, NULL);

  // Make surface click-through
  struct wl_region *input_region = wl_compositor_create_region(compositor);
  if (input_region) {
    wl_surface_set_input_region(surface, input_region);
    wl_region_destroy(input_region);
  }

  wl_surface_commit(surface);
  return BONGOCAT_SUCCESS;
}

static bongocat_error_t wayland_setup_buffer(void) {
  int size = current_config->screen_width * current_config->bar_height * 4;
  if (size <= 0) {
    bongocat_log_error("Invalid buffer size: %d", size);
    return BONGOCAT_ERROR_WAYLAND;
  }

  int fd = create_shm(size);
  if (fd < 0) {
    return BONGOCAT_ERROR_WAYLAND;
  }

  pixels =
      (uint8_t *)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (pixels == MAP_FAILED) {
    bongocat_log_error("Failed to map shared memory: %s", strerror(errno));
    close(fd);
    return BONGOCAT_ERROR_MEMORY;
  }

  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
  if (!pool) {
    bongocat_log_error("Failed to create shared memory pool");
    munmap(pixels, size);
    pixels = NULL;
    close(fd);
    return BONGOCAT_ERROR_WAYLAND;
  }

  buffer = wl_shm_pool_create_buffer(
      pool, 0, current_config->screen_width, current_config->bar_height,
      current_config->screen_width * 4, WL_SHM_FORMAT_ARGB8888);
  if (!buffer) {
    bongocat_log_error("Failed to create buffer");
    wl_shm_pool_destroy(pool);
    munmap(pixels, size);
    pixels = NULL;
    close(fd);
    return BONGOCAT_ERROR_WAYLAND;
  }

  wl_shm_pool_destroy(pool);
  close(fd);
  return BONGOCAT_SUCCESS;
}

bongocat_error_t wayland_init(config_t *config) {
  BONGOCAT_CHECK_NULL(config, BONGOCAT_ERROR_INVALID_PARAM);

  current_config = config;
  bongocat_log_info("Initializing Wayland connection");

  // Initialize random seed for shared memory name generation
  srand((unsigned int)time(NULL));

  display = wl_display_connect(NULL);
  if (!display) {
    bongocat_log_error("Failed to connect to Wayland display");
    return BONGOCAT_ERROR_WAYLAND;
  }

  bongocat_error_t result;
  if ((result = wayland_setup_protocols()) != BONGOCAT_SUCCESS ||
      (result = wayland_setup_surface()) != BONGOCAT_SUCCESS ||
      (result = wayland_setup_buffer()) != BONGOCAT_SUCCESS) {
    wayland_cleanup();
    return result;
  }

  bongocat_log_info("Wayland initialization complete (%dx%d buffer)",
                    current_config->screen_width, current_config->bar_height);
  return BONGOCAT_SUCCESS;
}

bongocat_error_t wayland_run(volatile sig_atomic_t *running) {
  BONGOCAT_CHECK_NULL(running, BONGOCAT_ERROR_INVALID_PARAM);

  bongocat_log_info("Starting Wayland event loop");

  while (*running && display) {
    // Handle Wayland events
    struct pollfd pfd = {
        .fd = wl_display_get_fd(display),
        .events = POLLIN,
    };

    while (wl_display_prepare_read(display) != 0) {
      if (wl_display_dispatch_pending(display) == -1) {
        bongocat_log_error("Failed to dispatch pending events");
        return BONGOCAT_ERROR_WAYLAND;
      }
    }

    int poll_result = poll(&pfd, 1, 100);

    if (poll_result > 0) {
      if (wl_display_read_events(display) == -1 ||
          wl_display_dispatch_pending(display) == -1) {
        bongocat_log_error("Failed to handle Wayland events");
        return BONGOCAT_ERROR_WAYLAND;
      }
    } else if (poll_result == 0) {
      wl_display_cancel_read(display);
    } else {
      wl_display_cancel_read(display);
      if (errno != EINTR) {
        bongocat_log_error("Poll error: %s", strerror(errno));
        return BONGOCAT_ERROR_WAYLAND;
      }
    }

    wl_display_flush(display);
  }

  bongocat_log_info("Wayland event loop exited");
  return BONGOCAT_SUCCESS;
}

// =============================================================================
// PUBLIC API IMPLEMENTATION
// =============================================================================

int wayland_get_screen_width(void) {
  return current_screen_info ? current_screen_info->screen_width : 0;
}

const char *wayland_get_output_name(void) {
  return bound_screen_name;
}

void wayland_update_config(config_t *config) {
  if (!config) {
    bongocat_log_error("Cannot update wayland config: config is NULL");
    return;
  }

  // Lock animation mutex to prevent draw_bar() during config update
  // This is critical - animation thread must not access buffer while we
  // recreate it
  pthread_mutex_lock(&anim_lock);

  // Check if dimensions changed - requires buffer/surface recreation
  int old_height = current_config ? current_config->bar_height : 0;
  int old_width = current_config ? current_config->screen_width : 0;
  const char *old_screen_name = current_config ? bound_screen_name : NULL;
  char *old_output_name = current_config && current_config->output_name
                              ? strdup(current_config->output_name)
                              : NULL;

  current_config = config;
  int new_width = wayland_get_new_screen_width();

  bool dimensions_changed = (old_height != config->bar_height) ||
                            (old_width != config->screen_width) ||
                            (new_width != config->screen_width);
  bool screen_changed =
      (current_config && current_config->output_name && old_screen_name &&
           (strcmp(old_screen_name, current_config->output_name) != 0) ||
       (current_config && current_config->output_name && old_output_name &&
        strcmp(old_output_name, current_config->output_name) != 0));

  if ((dimensions_changed && old_height > 0 && old_width > 0) ||
      screen_changed) {
    bongocat_log_info(
        "Dimensions changed (%dx%d -> %dx%d), recreating buffer...", old_width,
        old_height, new_width, config->bar_height);

    // Mark as not configured first
    atomic_store(&configured, false);

    // Cleanup old buffer
    if (buffer) {
      wl_buffer_destroy(buffer);
      buffer = NULL;
    }
    if (pixels) {
      munmap(pixels, old_width * old_height * 4);
      pixels = NULL;
    }

    // Cleanup old surface
    if (layer_surface) {
      zwlr_layer_surface_v1_destroy(layer_surface);
      layer_surface = NULL;
    }
    if (surface) {
      wl_surface_destroy(surface);
      surface = NULL;
    }

    wayland_update_output();
    wayland_update_current_screen_info();

    // Recreate surface and buffer with new dimensions
    if (wayland_setup_surface() != BONGOCAT_SUCCESS) {
      bongocat_log_error("Failed to recreate surface after config change");
      if (old_output_name != NULL) {
        free(old_output_name);
        old_output_name = NULL;
      }
      pthread_mutex_unlock(&anim_lock);
      return;
    }

    wayland_update_output();
    wayland_update_current_screen_info();

    if (wayland_setup_buffer() != BONGOCAT_SUCCESS) {
      bongocat_log_error("Failed to recreate buffer after config change");
      if (old_output_name != NULL) {
        free(old_output_name);
        old_output_name = NULL;
      }
      pthread_mutex_unlock(&anim_lock);
      return;
    }

    // Wait for new configure event
    wl_display_roundtrip(display);
    wayland_update_current_screen_info();

    bongocat_log_info("Buffer recreated successfully (%dx%d)",
                      config->screen_width, config->bar_height);
  }
  if (old_output_name != NULL) {
    free(old_output_name);
    old_output_name = NULL;
  }

  pthread_mutex_unlock(&anim_lock);

  if (atomic_load(&configured)) {
    draw_bar();
  }
}

void wayland_cleanup(void) {
  bongocat_log_info("Cleaning up Wayland resources");

  // First destroy xdg_output objects
  for (size_t i = 0; i < output_count; ++i) {
    if (outputs[i].xdg_output) {
      bongocat_log_debug("Destroying xdg_output %zu", i);
      zxdg_output_v1_destroy(outputs[i].xdg_output);
      outputs[i].xdg_output = NULL;
    }
  }

  // Then destroy the manager
  if (xdg_output_manager) {
    bongocat_log_debug("Destroying xdg_output_manager");
    zxdg_output_manager_v1_destroy(xdg_output_manager);
    xdg_output_manager = NULL;
  }

  // Finally destroy wl_output objects
  for (size_t i = 0; i < output_count; ++i) {
    if (outputs[i].wl_output) {
      bongocat_log_debug("Destroying wl_output %zu", i);
      wl_output_destroy(outputs[i].wl_output);
      outputs[i].wl_output = NULL;
    }
  }

  output_count = 0;

  if (buffer) {
    wl_buffer_destroy(buffer);
    buffer = NULL;
  }

  if (pixels && current_config) {
    int size = current_config->screen_width * current_config->bar_height * 4;
    munmap(pixels, size);
    pixels = NULL;
  }

  if (layer_surface) {
    zwlr_layer_surface_v1_destroy(layer_surface);
    layer_surface = NULL;
  }

  if (surface) {
    wl_surface_destroy(surface);
    surface = NULL;
  }

  // Note: output is just a reference to one of the outputs[] entries
  // It will be destroyed when we destroy the outputs[] array above
  output = NULL;

  if (layer_shell) {
    zwlr_layer_shell_v1_destroy(layer_shell);
    layer_shell = NULL;
  }

  if (xdg_wm_base) {
    xdg_wm_base_destroy(xdg_wm_base);
    xdg_wm_base = NULL;
  }

  if (fs_detector.manager) {
    zwlr_foreign_toplevel_manager_v1_destroy(fs_detector.manager);
    fs_detector.manager = NULL;
  }

  if (shm) {
    wl_shm_destroy(shm);
    shm = NULL;
  }

  if (compositor) {
    wl_compositor_destroy(compositor);
    compositor = NULL;
  }

  if (display) {
    wl_display_disconnect(display);
    display = NULL;
  }

  // Reset state
  atomic_store(&configured, false);
  atomic_store(&fullscreen_detected, false);
  atomic_store(&output_lost, false);
  bound_output_name = 0;
  using_named_output = false;
  global_registry = NULL;  // Destroyed when display disconnects
  bound_screen_name = NULL;
  current_screen_info = NULL;
  memset(&fs_detector, 0, sizeof(fs_detector));
  memset(&screen_infos, 0, sizeof(screen_info_t) * MAX_OUTPUTS);

  bongocat_log_debug("Wayland cleanup complete");
}

const char *wayland_get_current_layer_name(void) {
  return "OVERLAY";
}
