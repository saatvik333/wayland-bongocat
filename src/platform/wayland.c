#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "platform/wayland.h"

#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#endif
#include "../protocols/fractional-scale-v1-client-protocol.h"
#include "../protocols/viewporter-client-protocol.h"
#include "../protocols/wlr-foreign-toplevel-management-v1-client-protocol.h"
#include "../protocols/xdg-output-unstable-v1-client-protocol.h"
#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
#include "graphics/animation.h"
#include "platform/fullscreen.h"
#include "platform/hyprland.h"
#include "platform/scale.h"

#include <poll.h>
#include <signal.h>
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
struct wl_output *output;
struct wl_surface *surface;
struct zwlr_layer_surface_v1 *layer_surface;

typedef struct buffer_slot {
  struct wl_buffer *buffer;
  uint8_t *pixels;
  size_t size;
  atomic_bool busy;
  bool retired;
  struct buffer_slot *next;
} buffer_slot_t;

static buffer_slot_t *active_buffers[2] = {0};
static buffer_slot_t *retired_buffers = NULL;

// HiDPI: fractional-scale + viewporter
static struct wp_viewporter *viewporter = NULL;
static struct wp_fractional_scale_manager_v1 *fractional_scale_mgr = NULL;
static struct wp_viewport *viewport = NULL;
static struct wp_fractional_scale_v1 *fractional_scale_obj = NULL;
// Effective render scale, encoded as numerator over 120 (so 120 = 1.0×, 240 =
// 2.0×, 180 = 1.5×). Updated via wp_fractional_scale_v1::preferred_scale or
// wl_output::scale fallback.
static uint32_t current_scale_120 = 120;
static atomic_uint pending_scale_120 = 120;
static atomic_bool scale_change_pending = false;
static atomic_bool output_change_pending = false;
static atomic_bool redraw_pending = false;
// Physical (buffer-coordinate) dimensions of the active buffer.
static int physical_buffer_w = 0;
static int physical_buffer_h = 0;

// Ceil-divide logical pixels by 120 / scale_120 to get physical pixels.
static inline int phys_dim(int logical) {
  return scale_size_120(logical, current_scale_120);
}

int wayland_phys_dim(int logical) {
  return phys_dim(logical);
}

static config_t *current_config;
static void (*tick_callback_fn)(void) = NULL;
static int applied_width = 0;
static int applied_height = 0;
static layer_type_t applied_layer = LAYER_TOP;
static uint32_t layer_shell_version = 0;
static overlay_position_t applied_position = POSITION_BOTTOM;
static char *applied_output_name = NULL;

static uint32_t wayland_layer_value(layer_type_t layer) {
  switch (layer) {
  case LAYER_BACKGROUND:
    return ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
  case LAYER_BOTTOM:
    return ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
  case LAYER_OVERLAY:
    return ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
  case LAYER_TOP:
  default:
    return ZWLR_LAYER_SHELL_V1_LAYER_TOP;
  }
}

static const char *wayland_layer_name(layer_type_t layer) {
  switch (layer) {
  case LAYER_BACKGROUND:
    return "background";
  case LAYER_BOTTOM:
    return "bottom";
  case LAYER_OVERLAY:
    return "overlay";
  case LAYER_TOP:
  default:
    return "top";
  }
}

// =============================================================================
// SCREEN DIMENSION MANAGEMENT
// =============================================================================

output_ref_t outputs[MAX_OUTPUTS];
size_t output_count = 0;
static struct zxdg_output_manager_v1 *xdg_output_manager = NULL;
static output_ref_t *current_output_info = NULL;

// Output reconnection handling
static struct wl_registry *global_registry = NULL;
static uint32_t bound_output_name = 0;   // Registry name of our bound output
static atomic_bool output_lost = false;  // Set when our output disconnects
static bool using_named_output =
    false;  // True if user specified an output name
static char *bound_screen_name = NULL;

BONGOCAT_NODISCARD static struct wl_output *wayland_find_new_output(void) {
  if (current_config->output_name) {
    for (size_t i = 0; i < MAX_OUTPUTS; ++i) {
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
  for (size_t i = 0; i < MAX_OUTPUTS; ++i) {
    if (outputs[i].wl_output == matching_wl_output) {
      return outputs[i].screen_width;
    }
  }

  return 0;
}

static void wayland_update_current_output_info(void) {
  bool output_found = false;
  if (output) {
    for (size_t i = 0; i < MAX_OUTPUTS; i++) {
      if (outputs[i].wl_output == output) {
        bongocat_log_info("Detected screen name: %s", outputs[i].name_str);
        bound_screen_name = outputs[i].name_str;
      }
      if (outputs[i].wl_output == output) {
        if (outputs[i].screen_width > 0 && outputs[i].screen_width <= 32768) {
          bongocat_log_info("Detected screen width: %d",
                            outputs[i].screen_width);
          current_output_info = &outputs[i];
          current_config->screen_width = outputs[i].screen_width;
          output_found = true;
        }
      }
    }
  }

  if (!output_found) {
    bongocat_log_warning("No output found, using default screen width: %d",
                         DEFAULT_SCREEN_WIDTH);
    current_config->screen_width = DEFAULT_SCREEN_WIDTH;
    current_output_info = NULL;
  }
}

// =============================================================================
// ZXDG LISTENER IMPLEMENTATION
// =============================================================================

// Forward declarations for reconnection handling
static bongocat_error_t wayland_setup_surface(void);
static void screen_calculate_dimensions(output_ref_t *screen_info);

static void
handle_xdg_output_name(void *data,
                       [[maybe_unused]] struct zxdg_output_v1 *xdg_output,
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
      wayland_update_current_output_info();
      bongocat_log_info("Surface recreated, configure event processed");
    } else {
      bongocat_log_error("Failed to recreate surface on reconnected output");
    }
  }
}

static void handle_xdg_output_logical_position(
    void *data, [[maybe_unused]] struct zxdg_output_v1 *xdg_output, int32_t x,
    int32_t y) {
  // Defensive null check
  if (!data) {
    return;
  }

  output_ref_t *oref = data;

  oref->x = x;
  oref->y = y;

  bongocat_log_debug("xdg-output logical position received: %d,%d", x, y);
}
static void handle_xdg_output_logical_size(
    void *data, [[maybe_unused]] struct zxdg_output_v1 *xdg_output,
    int32_t width, int32_t height) {
  // Defensive null check
  if (!data) {
    return;
  }

  output_ref_t *oref = data;

  oref->width = width;
  oref->height = height;
  screen_calculate_dimensions(oref);
  atomic_store(&output_change_pending, true);

  bongocat_log_debug("xdg-output logical size received: %dx%d", width, height);
}
static void
handle_xdg_output_done([[maybe_unused]] void *data,
                       [[maybe_unused]] struct zxdg_output_v1 *xdg_output) {}

static void handle_xdg_output_description(
    [[maybe_unused]] void *data,
    [[maybe_unused]] struct zxdg_output_v1 *xdg_output,
    [[maybe_unused]] const char *description) {}

static const struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_position = handle_xdg_output_logical_position,
    .logical_size = handle_xdg_output_logical_size,
    .done = handle_xdg_output_done,
    .name = handle_xdg_output_name,
    .description = handle_xdg_output_description};

// =============================================================================
// SCREEN DIMENSION MANAGEMENT
// =============================================================================

static void screen_calculate_dimensions(output_ref_t *screen_info) {
  if (!screen_info) {
    return;
  }

  if ((!screen_info->mode_received || !screen_info->geometry_received) &&
      (screen_info->width <= 0 || screen_info->height <= 0)) {
    return;
  }
  output_logical_size(screen_info->raw_width, screen_info->raw_height,
                      screen_info->transform, screen_info->wl_scale,
                      screen_info->width, screen_info->height,
                      &screen_info->screen_width, &screen_info->screen_height);
}

// =============================================================================
// BUFFER AND DRAWING MANAGEMENT
// =============================================================================

static int create_shm(int size) {
  int fd = memfd_create("bongocat-shm", MFD_CLOEXEC);
  if (fd < 0) {
    bongocat_log_error("memfd_create failed: %s", strerror(errno));
    return -1;
  }

  if (ftruncate(fd, size) < 0) {
    bongocat_log_error("ftruncate failed: %s", strerror(errno));
    close(fd);
    return -1;
  }

  return fd;
}

static void buffer_released(void *data,
                            [[maybe_unused]] struct wl_buffer *wl_buffer) {
  buffer_slot_t *slot = data;
  atomic_store_explicit(&slot->busy, false, memory_order_release);
  if (slot->retired)
    atomic_store(&redraw_pending, true);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_released,
};

static void destroy_buffer_slot(buffer_slot_t *slot) {
  if (!slot)
    return;
  if (slot->buffer)
    wl_buffer_destroy(slot->buffer);
  if (slot->pixels && slot->size > 0)
    munmap(slot->pixels, slot->size);
  free(slot);
}

static void cleanup_retired_buffers(void) {
  pthread_mutex_lock(&anim_lock);
  buffer_slot_t **slot = &retired_buffers;
  while (*slot) {
    if (!atomic_load_explicit(&(*slot)->busy, memory_order_acquire)) {
      buffer_slot_t *released = *slot;
      *slot = released->next;
      destroy_buffer_slot(released);
    } else {
      slot = &(*slot)->next;
    }
  }
  pthread_mutex_unlock(&anim_lock);
}

static void retire_active_buffers(void) {
  for (size_t i = 0; i < 2; i++) {
    buffer_slot_t *slot = active_buffers[i];
    active_buffers[i] = NULL;
    if (!slot)
      continue;
    slot->retired = true;
    slot->next = retired_buffers;
    retired_buffers = slot;
  }
}

void draw_bar(void) {
  if (!atomic_load(&configured)) {
    bongocat_log_debug("Surface not configured yet, skipping draw");
    return;
  }

  pthread_mutex_lock(&anim_lock);

  buffer_slot_t *slot = NULL;
  for (size_t i = 0; i < 2; i++) {
    if (active_buffers[i] &&
        !atomic_exchange_explicit(&active_buffers[i]->busy, true,
                                  memory_order_acq_rel)) {
      slot = active_buffers[i];
      break;
    }
  }

  if (!current_config || !surface || !slot) {
    if (slot)
      atomic_store(&slot->busy, false);
    atomic_store(&redraw_pending, true);
    pthread_mutex_unlock(&anim_lock);
    return;
  }

  // Skip fullscreen hiding when layer is LAYER_OVERLAY (always visible)
  bool is_overlay_layer = current_config->layer == LAYER_OVERLAY;
  bool is_fullscreen = !is_overlay_layer &&
                       !current_config->disable_fullscreen_hide &&
                       atomic_load(&fullscreen_detected);
  int effective_opacity = is_fullscreen ? 0 : current_config->overlay_opacity;

  // Clear buffer with transparency - OPTIMIZED
  // Write all pixels as 32-bit values: RGB=0, A=opacity
  // Buffer dimensions are physical (post-scale) pixels — the compositor maps
  // them back to the logical surface size via wp_viewport / buffer_scale.
  int phys_w = physical_buffer_w;
  int phys_h = physical_buffer_h;
  size_t buffer_size = (size_t)phys_w * (size_t)phys_h * 4U;
  memset(slot->pixels, 0, buffer_size);

  if (effective_opacity > 0) {
    uint32_t fill = (uint32_t)effective_opacity << 24;
    uint32_t *px = (uint32_t *)slot->pixels;
    size_t pixel_count = buffer_size / 4;
    for (size_t i = 0; i < pixel_count; i++) {
      px[i] = fill;
    }
  }

  // Draw cat if visible
  if (!is_fullscreen) {
    // Cat dimensions and offsets are in logical pixels in the config; convert
    // to physical for the buffer-space blit.
    int cat_height_phys = phys_dim(current_config->cat_height);
    int cat_width_phys = (cat_height_phys * CAT_IMAGE_WIDTH) / CAT_IMAGE_HEIGHT;
    int cat_y_phys =
        (phys_h - cat_height_phys) / 2 +
        scale_offset_120(current_config->cat_y_offset, current_scale_120);

    int cat_x_phys = 0;
    switch (current_config->cat_align) {
    case ALIGN_CENTER:
      cat_x_phys =
          (phys_w - cat_width_phys) / 2 +
          scale_offset_120(current_config->cat_x_offset, current_scale_120);
      break;
    case ALIGN_LEFT:
      cat_x_phys =
          scale_offset_120(current_config->cat_x_offset, current_scale_120);
      break;
    case ALIGN_RIGHT:
      cat_x_phys =
          phys_w - cat_width_phys -
          scale_offset_120(current_config->cat_x_offset, current_scale_120);
      break;
    }

    cached_frame_t *frame = &anim_cached_frames[anim_index];
    if (frame->data && frame->width > 0 && frame->height > 0) {
      // Blit pre-scaled cached frame (already BGRA, no channel swap)
      blit_cached_frame(slot->pixels, phys_w, phys_h, frame->data, frame->width,
                        frame->height, cat_x_phys, cat_y_phys);
    } else {
      bongocat_log_debug("Frame %d cache not ready, skipping draw", anim_index);
    }
  } else {
    bongocat_log_debug("Cat hidden due to fullscreen detection");
  }

  wl_surface_attach(surface, slot->buffer, 0, 0);
  wl_surface_damage_buffer(surface, 0, 0, phys_w, phys_h);
  wl_surface_commit(surface);
  pthread_mutex_unlock(&anim_lock);

  // Flush outside the lock -- may block on write() syscall
  wl_display_flush(display);
}

void wayland_request_redraw(void) {
  atomic_store(&redraw_pending, true);
}

// =============================================================================
// WAYLAND EVENT HANDLERS
// =============================================================================

static void layer_surface_configure([[maybe_unused]] void *data,
                                    struct zwlr_layer_surface_v1 *ls,
                                    uint32_t serial, uint32_t w, uint32_t h) {
  bongocat_log_debug("Layer surface configured: %dx%d", w, h);
  zwlr_layer_surface_v1_ack_configure(ls, serial);
  atomic_store(&configured, true);
  atomic_store(&redraw_pending, true);
}

// Handle compositor-requested surface closure
static void
layer_surface_closed([[maybe_unused]] void *data,
                     [[maybe_unused]] struct zwlr_layer_surface_v1 *ls) {
  bongocat_log_info("Layer surface closed by compositor");
  atomic_store(&configured, false);
}

static struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

static void
output_geometry([[maybe_unused]] void *data, struct wl_output *wl_output,
                [[maybe_unused]] int32_t x, [[maybe_unused]] int32_t y,
                [[maybe_unused]] int32_t physical_width,
                [[maybe_unused]] int32_t physical_height,
                [[maybe_unused]] int32_t subpixel,
                [[maybe_unused]] const char *make,
                [[maybe_unused]] const char *model, int32_t transform) {
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    if (outputs[i].wl_output == wl_output) {
      outputs[i].transform = transform;
      outputs[i].geometry_received = true;
      bongocat_log_debug("Output transform: %d", transform);
      screen_calculate_dimensions(&outputs[i]);
      atomic_store(&output_change_pending, true);
      break;
    }
  }
}

static void output_mode([[maybe_unused]] void *data,
                        struct wl_output *wl_output, uint32_t flags,
                        int32_t width, int32_t height,
                        [[maybe_unused]] int32_t refresh) {
  if (flags & WL_OUTPUT_MODE_CURRENT) {
    for (size_t i = 0; i < MAX_OUTPUTS; i++) {
      if (outputs[i].wl_output == wl_output) {
        outputs[i].raw_width = width;
        outputs[i].raw_height = height;
        outputs[i].mode_received = true;
        bongocat_log_debug("Received raw screen mode: %dx%d", width, height);
        screen_calculate_dimensions(&outputs[i]);
        atomic_store(&output_change_pending, true);
        break;
      }
    }
  }
}

static void output_done([[maybe_unused]] void *data,
                        struct wl_output *wl_output) {
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    if (outputs[i].wl_output == wl_output) {
      screen_calculate_dimensions(&outputs[i]);
      atomic_store(&output_change_pending, true);
      bongocat_log_debug("Output configuration complete");
      break;
    }
  }
}

static void output_scale([[maybe_unused]] void *data,
                         struct wl_output *wl_output, int32_t factor) {
  if (factor < 1)
    factor = 1;
  for (size_t i = 0; i < MAX_OUTPUTS; ++i) {
    if (outputs[i].wl_output == wl_output) {
      outputs[i].wl_scale = factor;
      screen_calculate_dimensions(&outputs[i]);
      atomic_store(&output_change_pending, true);
      if (!fractional_scale_obj && outputs[i].wl_output == output) {
        atomic_store(&pending_scale_120, (unsigned)factor * 120u);
        atomic_store(&scale_change_pending, true);
      }
      break;
    }
  }
}

static struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
};

// =============================================================================
// HIDPI: FRACTIONAL SCALE HANDLING
// =============================================================================

// Forward declarations.
static bongocat_error_t wayland_recreate_buffer_for_scale(void);
static void wayland_recache_frames_for_scale(void);

static void fractional_scale_preferred_scale(
    [[maybe_unused]] void *data,
    [[maybe_unused]] struct wp_fractional_scale_v1 *fs, uint32_t scale) {
  if (scale == 0 || scale == current_scale_120) {
    return;
  }
  atomic_store(&pending_scale_120, scale);
  atomic_store(&scale_change_pending, true);
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener =
    {
        .preferred_scale = fractional_scale_preferred_scale,
};

// Pick effective scale_120 from output's wl_output::scale when fractional
// protocol is unavailable.
static uint32_t scale_120_from_output(void) {
  if (current_output_info && current_output_info->wl_scale > 0) {
    return (uint32_t)current_output_info->wl_scale * 120u;
  }
  return 120;
}

// =============================================================================
// WAYLAND PROTOCOL REGISTRY
// =============================================================================

static void registry_global([[maybe_unused]] void *data,
                            struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t ver) {
#define BIND_MIN_VER(v, desired) ((v) < (desired) ? (v) : (desired))

  if (strcmp(iface, wl_compositor_interface.name) == 0) {
    compositor = (struct wl_compositor *)wl_registry_bind(
        reg, name, &wl_compositor_interface, BIND_MIN_VER(ver, 4));
  } else if (strcmp(iface, wl_shm_interface.name) == 0) {
    shm = (struct wl_shm *)wl_registry_bind(reg, name, &wl_shm_interface,
                                            BIND_MIN_VER(ver, 1));
  } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
    layer_shell_version = BIND_MIN_VER(ver, 4);
    layer_shell = (struct zwlr_layer_shell_v1 *)wl_registry_bind(
        reg, name, &zwlr_layer_shell_v1_interface, layer_shell_version);
  } else if (strcmp(iface, zxdg_output_manager_v1_interface.name) == 0) {
    xdg_output_manager = wl_registry_bind(
        reg, name, &zxdg_output_manager_v1_interface, BIND_MIN_VER(ver, 3));
  } else if (strcmp(iface, wp_viewporter_interface.name) == 0) {
    viewporter = (struct wp_viewporter *)wl_registry_bind(
        reg, name, &wp_viewporter_interface, BIND_MIN_VER(ver, 1));
  } else if (strcmp(iface, wp_fractional_scale_manager_v1_interface.name) ==
             0) {
    fractional_scale_mgr =
        (struct wp_fractional_scale_manager_v1 *)wl_registry_bind(
            reg, name, &wp_fractional_scale_manager_v1_interface,
            BIND_MIN_VER(ver, 1));
  } else if (strcmp(iface, wl_output_interface.name) == 0) {
    if (output_count < MAX_OUTPUTS) {
      size_t slot = 0;
      while (slot < MAX_OUTPUTS && outputs[slot].wl_output)
        slot++;
      if (slot == MAX_OUTPUTS)
        return;
      outputs[slot].name = name;
      outputs[slot].wl_scale = 1;
      outputs[slot].hypr_id = -1;
      outputs[slot].wl_output = wl_registry_bind(
          reg, name, &wl_output_interface, BIND_MIN_VER(ver, 2));
      wl_output_add_listener(outputs[slot].wl_output, &output_listener, NULL);

      // If we lost our output, get xdg_output to check if this is the one
      // reconnecting
      if (atomic_load(&output_lost) && xdg_output_manager) {
        outputs[slot].xdg_output = zxdg_output_manager_v1_get_xdg_output(
            xdg_output_manager, outputs[slot].wl_output);
        outputs[slot].name_received = false;
        zxdg_output_v1_add_listener(outputs[slot].xdg_output,
                                    &xdg_output_listener, &outputs[slot]);
        bongocat_log_debug(
            "New output appeared while output_lost, checking name...");
      }

      output_count++;
    }
  } else if (strcmp(iface, zwlr_foreign_toplevel_manager_v1_interface.name) ==
             0) {
    struct zwlr_foreign_toplevel_manager_v1 *fs_manager =
        (struct zwlr_foreign_toplevel_manager_v1 *)wl_registry_bind(
            reg, name, &zwlr_foreign_toplevel_manager_v1_interface,
            BIND_MIN_VER(ver, 3));
    fullscreen_init(fs_manager);
  }

#undef BIND_MIN_VER
}

static void registry_remove([[maybe_unused]] void *data,
                            [[maybe_unused]] struct wl_registry *registry,
                            uint32_t name) {
  size_t removed_index = MAX_OUTPUTS;
  for (size_t i = 0; i < MAX_OUTPUTS; ++i) {
    if (outputs[i].name == name) {
      removed_index = i;
      break;
    }
  }

  if (removed_index == MAX_OUTPUTS) {
    return;
  }

  bool removed_bound = (name == bound_output_name && bound_output_name != 0);
  if (removed_bound) {
    bongocat_log_warning("Bound output disconnected (registry name %u)", name);
    atomic_store(&output_lost, true);
    atomic_store(&configured, false);
    output = NULL;
    bound_output_name = 0;
    bound_screen_name = NULL;
    current_output_info = NULL;
  }

  if (outputs[removed_index].xdg_output) {
    zxdg_output_v1_destroy(outputs[removed_index].xdg_output);
    outputs[removed_index].xdg_output = NULL;
  }
  if (outputs[removed_index].wl_output) {
    wl_output_destroy(outputs[removed_index].wl_output);
    outputs[removed_index].wl_output = NULL;
  }

  memset(&outputs[removed_index], 0, sizeof(output_ref_t));
  if (output_count > 0)
    output_count--;

  if (!removed_bound && output != NULL) {
    current_output_info = NULL;
    bound_screen_name = NULL;

    for (size_t i = 0; i < MAX_OUTPUTS; ++i) {
      if (outputs[i].wl_output == output) {
        bound_screen_name = outputs[i].name_str;
      }
      if (outputs[i].wl_output == output) {
        current_output_info = &outputs[i];
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
  current_output_info = NULL;
  bound_screen_name = NULL;

  if (current_config->output_name) {
    for (size_t i = 0; i < MAX_OUTPUTS; ++i) {
      if (outputs[i].name_received &&
          strcmp(outputs[i].name_str, current_config->output_name) == 0) {
        output = outputs[i].wl_output;
        bound_output_name =
            outputs[i].name;  // Store registry name for tracking
        bound_screen_name = outputs[i].name_str;
        using_named_output = true;  // User specified this output
        current_output_info = &outputs[i];
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
    for (size_t i = 0; i < MAX_OUTPUTS; i++) {
      if (outputs[i].wl_output) {
        output = outputs[i].wl_output;
        bound_output_name = outputs[i].name;
        bound_screen_name = outputs[i].name_str;
        current_output_info = &outputs[i];
        break;
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
    for (size_t i = 0; i < MAX_OUTPUTS; ++i) {
      if (!outputs[i].wl_output)
        continue;
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
    if (!compositor)
      bongocat_log_error("Missing protocol: wl_compositor");
    if (!shm)
      bongocat_log_error("Missing protocol: wl_shm");
    if (!layer_shell)
      bongocat_log_error("Missing protocol: wlr-layer-shell (required for "
                         "overlay rendering). Your compositor may not support "
                         "this protocol.");
    bongocat_log_error(
        "Cannot start: required Wayland protocols not available");
    wl_registry_destroy(global_registry);
    global_registry = NULL;
    return BONGOCAT_ERROR_WAYLAND;
  }

  // Warn about optional protocols
  if (!fs_detector_available()) {
    bongocat_log_warning("Foreign toplevel protocol not available — fullscreen "
                         "detection disabled. Overlay will not auto-hide when "
                         "apps go fullscreen.");
  }

  // Configure screen dimensions
  wayland_update_current_output_info();

  // Keep registry alive for output reconnection handling
  return BONGOCAT_SUCCESS;
}

static bongocat_error_t wayland_setup_surface(void) {
  if (!current_config) {
    bongocat_log_error("Cannot setup surface: config is NULL");
    return BONGOCAT_ERROR_INVALID_PARAM;
  }

  uint32_t wl_layer = wayland_layer_value(current_config->layer);

  surface = wl_compositor_create_surface(compositor);
  if (!surface) {
    bongocat_log_error("Failed to create surface");
    return BONGOCAT_ERROR_WAYLAND;
  }

  // HiDPI plumbing: pair surface with a viewport (so we can render at
  // physical-pixel resolution and let compositor downscale to the logical
  // size) and a fractional-scale receiver (so we learn the preferred scale).
  if (viewporter && !viewport) {
    viewport = wp_viewporter_get_viewport(viewporter, surface);
  }
  if (fractional_scale_mgr && !fractional_scale_obj) {
    fractional_scale_obj = wp_fractional_scale_manager_v1_get_fractional_scale(
        fractional_scale_mgr, surface);
    if (fractional_scale_obj) {
      wp_fractional_scale_v1_add_listener(fractional_scale_obj,
                                          &fractional_scale_listener, NULL);
    }
  }

  // If fractional protocol is unavailable, seed scale from wl_output integer
  // scale so the first buffer is sized correctly.
  if (!fractional_scale_obj) {
    current_scale_120 = scale_120_from_output();
  }

  layer_surface = zwlr_layer_shell_v1_get_layer_surface(
      layer_shell, surface, output, wl_layer, "bongocat-overlay");

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
  zwlr_layer_surface_v1_set_size(layer_surface, 0,
                                 current_config->overlay_height);
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
  int logical_w = current_config->screen_width;
  int logical_h = current_config->overlay_height;
  int phys_w = phys_dim(logical_w);
  int phys_h = phys_dim(logical_h);

  size_t size = (size_t)phys_w * (size_t)phys_h * 4U;
  if (size == 0 || size > (size_t)INT32_MAX) {
    bongocat_log_error("Invalid buffer size: %zu", size);
    return BONGOCAT_ERROR_WAYLAND;
  }

  buffer_slot_t *new_buffers[2] = {0};
  for (size_t i = 0; i < 2; i++) {
    int fd = create_shm((int)size);
    if (fd < 0)
      goto fail;

    buffer_slot_t *slot = calloc(1, sizeof(*slot));
    if (!slot) {
      close(fd);
      goto fail;
    }
    slot->pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    slot->size = size;
    if (slot->pixels == MAP_FAILED) {
      slot->pixels = NULL;
      close(fd);
      destroy_buffer_slot(slot);
      goto fail;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int)size);
    if (!pool) {
      close(fd);
      destroy_buffer_slot(slot);
      goto fail;
    }
    slot->buffer = wl_shm_pool_create_buffer(
        pool, 0, phys_w, phys_h, phys_w * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    if (!slot->buffer) {
      destroy_buffer_slot(slot);
      goto fail;
    }
    atomic_init(&slot->busy, false);
    wl_buffer_add_listener(slot->buffer, &buffer_listener, slot);
    new_buffers[i] = slot;
  }

  retire_active_buffers();
  active_buffers[0] = new_buffers[0];
  active_buffers[1] = new_buffers[1];

  physical_buffer_w = phys_w;
  physical_buffer_h = phys_h;

  // Tell the compositor how to map our physical buffer to the logical
  // surface. With viewporter we keep buffer_scale=1 and let the destination
  // size carry the logical dimensions (works for any fractional ratio).
  // Without viewporter, fall back to integer set_buffer_scale.
  if (viewport) {
    wp_viewport_set_destination(viewport, logical_w, logical_h);
    wl_surface_set_buffer_scale(surface, 1);
  } else {
    int integer_scale = (int)(current_scale_120 / 120u);
    if (integer_scale < 1)
      integer_scale = 1;
    wl_surface_set_buffer_scale(surface, integer_scale);
  }

  bongocat_log_debug(
      "Buffer allocated: logical %dx%d, physical %dx%d, scale %u/120",
      logical_w, logical_h, phys_w, phys_h, current_scale_120);
  return BONGOCAT_SUCCESS;

fail:
  destroy_buffer_slot(new_buffers[0]);
  destroy_buffer_slot(new_buffers[1]);
  return BONGOCAT_ERROR_MEMORY;
}

// Tear down current buffer + shm and reallocate at the active scale. Caller
// must hold anim_lock if there's any chance draw_bar is racing.
static bongocat_error_t wayland_recreate_buffer_for_scale(void) {
  if (!current_config || !surface) {
    return BONGOCAT_ERROR_INVALID_PARAM;
  }

  pthread_mutex_lock(&anim_lock);
  bongocat_error_t err = wayland_setup_buffer();
  pthread_mutex_unlock(&anim_lock);

  if (err != BONGOCAT_SUCCESS) {
    return err;
  }

  // Surface needs a commit so the compositor picks up the new buffer_scale /
  // viewport destination before the next draw.
  wl_surface_commit(surface);
  return BONGOCAT_SUCCESS;
}

// Re-rasterize the SVG cat frames at the new physical pixel resolution.
static void wayland_recache_frames_for_scale(void) {
  if (!current_config)
    return;
  pthread_mutex_lock(&anim_lock);
  animation_invalidate_cache();
  int cat_h_phys = phys_dim(current_config->cat_height);
  int cat_w_phys = (cat_h_phys * CAT_IMAGE_WIDTH) / CAT_IMAGE_HEIGHT;
  animation_cache_frames(cat_w_phys, cat_h_phys, current_config->mirror_x,
                         current_config->mirror_y,
                         current_config->enable_antialiasing);
  pthread_mutex_unlock(&anim_lock);
}

static void wayland_process_pending_changes(void) {
  cleanup_retired_buffers();

  if (atomic_exchange(&output_change_pending, false) && current_config &&
      output) {
    int old_width = current_config->screen_width;
    wayland_update_current_output_info();
    if (applied_width > 0 && current_config->screen_width != old_width)
      wayland_update_config(current_config);
  }

  if (atomic_exchange(&scale_change_pending, false)) {
    uint32_t scale = atomic_load(&pending_scale_120);
    if (scale > 0 && scale != current_scale_120) {
      bongocat_log_info("Applying render scale %u/120 (%.3f)", scale,
                        (double)scale / 120.0);
      current_scale_120 = scale;
      if (surface && wayland_recreate_buffer_for_scale() == BONGOCAT_SUCCESS) {
        wayland_recache_frames_for_scale();
        atomic_store(&redraw_pending, true);
      }
    }
  }

  if (atomic_exchange(&redraw_pending, false) && atomic_load(&configured))
    draw_bar();
}

bongocat_error_t wayland_init(config_t *config) {
  BONGOCAT_CHECK_NULL(config, BONGOCAT_ERROR_INVALID_PARAM);

  current_config = config;
  bongocat_log_info("Initializing Wayland connection");

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

  // Drain the pending fractional-scale handshake so the buffer is sized at
  // the compositor's preferred scale before main.c rasterizes the SVGs.
  // Without this, scale 2.0 displays would render the first frame at 1×.
  wl_display_roundtrip(display);
  wayland_process_pending_changes();

  applied_width = current_config->screen_width;
  applied_height = current_config->overlay_height;
  applied_layer = current_config->layer;
  applied_position = current_config->overlay_position;
  if (applied_output_name) {
    free(applied_output_name);
    applied_output_name = NULL;
  }
  if (current_config->output_name) {
    applied_output_name = strdup(current_config->output_name);
  }

  bongocat_log_info("Wayland initialization complete (%dx%d buffer)",
                    current_config->screen_width,
                    current_config->overlay_height);
  return BONGOCAT_SUCCESS;
}

bongocat_error_t wayland_run(volatile sig_atomic_t *running) {
  BONGOCAT_CHECK_NULL(running, BONGOCAT_ERROR_INVALID_PARAM);

  bongocat_log_info("Starting Wayland event loop");

  while (*running && display) {
    if (tick_callback_fn) {
      tick_callback_fn();
    }

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

    wayland_process_pending_changes();

    wl_display_flush(display);
  }

  bongocat_log_info("Wayland event loop exited");
  return BONGOCAT_SUCCESS;
}

// =============================================================================
// PUBLIC API IMPLEMENTATION
// =============================================================================

struct wl_output *wayland_get_current_screen_output(void) {
  return current_output_info ? current_output_info->wl_output : NULL;
}

void wayland_set_tick_callback(void (*callback)(void)) {
  tick_callback_fn = callback;
}

// Apply double-buffered layer surface properties without destroying surfaces
static void apply_layer_properties(const config_t *config, bool do_position,
                                   bool do_layer) {
  if (do_position) {
    uint32_t anchor =
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    if (config->overlay_position == POSITION_TOP) {
      anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
    } else {
      anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
    }
    zwlr_layer_surface_v1_set_anchor(layer_surface, anchor);
    bongocat_log_info("Overlay position changed to %s",
                      config->overlay_position == POSITION_TOP ? "top"
                                                               : "bottom");
  }
  if (do_layer) {
    if (layer_shell_version >= ZWLR_LAYER_SURFACE_V1_SET_LAYER_SINCE_VERSION) {
      zwlr_layer_surface_v1_set_layer(layer_surface,
                                      wayland_layer_value(config->layer));
      bongocat_log_info("Layer changed to %s",
                        wayland_layer_name(config->layer));
    }
  }
}

void wayland_update_config(config_t *config) {
  if (!config) {
    bongocat_log_error("Cannot update wayland config: config is NULL");
    return;
  }

  current_config = config;

  int old_height = applied_height;
  int old_width = applied_width;
  layer_type_t old_layer = applied_layer;
  char *old_output_name =
      applied_output_name ? strdup(applied_output_name) : NULL;
  int new_width = wayland_get_new_screen_width();
  if (new_width > 0)
    config->screen_width = new_width;

  bool dimensions_changed = (old_height != config->overlay_height) ||
                            (old_width != config->screen_width);

  bool layer_changed = (old_layer != config->layer);
  bool position_changed = (applied_position != config->overlay_position);
  bool output_name_changed =
      ((old_output_name == NULL) != (config->output_name == NULL)) ||
      (old_output_name && config->output_name &&
       strcmp(old_output_name, config->output_name) != 0);
  bool bound_output_changed =
      (bound_screen_name && config->output_name &&
       strcmp(bound_screen_name, config->output_name) != 0);
  bool screen_changed = output_name_changed || bound_output_changed;

  // Determine which update path to use:
  // - Full recreate: only for output (monitor) changes
  // - Buffer recreate: for dimension changes (overlay_height, screen_width)
  // - Property update: for position/layer changes (double-buffered, no
  // recreate)
  // - Cache only: for cat_height, mirror, etc.
  bool needs_full_recreate =
      screen_changed ||
      (layer_changed &&
       layer_shell_version < ZWLR_LAYER_SURFACE_V1_SET_LAYER_SINCE_VERSION);
  bool needs_buffer_recreate =
      dimensions_changed && old_height > 0 && old_width > 0;
  bool needs_property_update = layer_changed || position_changed;

  if (needs_full_recreate) {
    // PATH 3: Output changed — full surface recreation required
    bongocat_log_info("Output changed, recreating surface");

    pthread_mutex_lock(&anim_lock);
    atomic_store(&configured, false);

    if (layer_surface) {
      zwlr_layer_surface_v1_destroy(layer_surface);
      layer_surface = NULL;
    }
    // Per-surface HiDPI receivers must die with the surface; setup_surface
    // will recreate them attached to the new wl_surface.
    if (fractional_scale_obj) {
      wp_fractional_scale_v1_destroy(fractional_scale_obj);
      fractional_scale_obj = NULL;
    }
    if (viewport) {
      wp_viewport_destroy(viewport);
      viewport = NULL;
    }
    if (surface) {
      wl_surface_destroy(surface);
      surface = NULL;
    }

    wayland_update_output();
    wayland_update_current_output_info();

    if (wayland_setup_surface() != BONGOCAT_SUCCESS) {
      bongocat_log_error("Failed to recreate surface after output change");
      pthread_mutex_unlock(&anim_lock);
      free(old_output_name);
      return;
    }

    wayland_update_output();
    wayland_update_current_output_info();

    if (wayland_setup_buffer() != BONGOCAT_SUCCESS) {
      bongocat_log_error("Failed to recreate buffer after output change");
      pthread_mutex_unlock(&anim_lock);
      free(old_output_name);
      return;
    }

    animation_invalidate_cache();
    int cat_h = phys_dim(config->cat_height);
    int cat_w = (cat_h * CAT_IMAGE_WIDTH) / CAT_IMAGE_HEIGHT;
    animation_cache_frames(cat_w, cat_h, config->mirror_x, config->mirror_y,
                           config->enable_antialiasing);

    pthread_mutex_unlock(&anim_lock);
    wl_display_roundtrip(display);
    wayland_update_current_output_info();

    bongocat_log_info("Surface recreated successfully (%dx%d)",
                      config->screen_width, config->overlay_height);

  } else if (needs_buffer_recreate) {
    // PATH 2: Dimensions changed — update size property, recreate buffer only
    bongocat_log_info("Overlay dimensions changed (%dx%d -> %dx%d)", old_width,
                      old_height, config->screen_width, config->overlay_height);

    // Update double-buffered properties on existing layer surface
    zwlr_layer_surface_v1_set_size(layer_surface, 0, config->overlay_height);
    apply_layer_properties(config, position_changed, layer_changed);
    wl_surface_commit(surface);

    // Recreate buffer under lock
    pthread_mutex_lock(&anim_lock);
    atomic_store(&configured, false);

    if (wayland_setup_buffer() != BONGOCAT_SUCCESS) {
      bongocat_log_error("Failed to recreate buffer after resize");
      pthread_mutex_unlock(&anim_lock);
      free(old_output_name);
      return;
    }

    animation_invalidate_cache();
    int cat_h = phys_dim(config->cat_height);
    int cat_w = (cat_h * CAT_IMAGE_WIDTH) / CAT_IMAGE_HEIGHT;
    animation_cache_frames(cat_w, cat_h, config->mirror_x, config->mirror_y,
                           config->enable_antialiasing);

    pthread_mutex_unlock(&anim_lock);

    // Roundtrip triggers configure callback → configured=true → draw_bar()
    wl_display_roundtrip(display);

    bongocat_log_info("Buffer resized successfully (%dx%d)",
                      config->screen_width, config->overlay_height);

  } else if (needs_property_update) {
    // PATH 1: Position/layer only — no buffer changes needed
    apply_layer_properties(config, position_changed, layer_changed);
    wl_surface_commit(surface);
    wl_display_roundtrip(display);
  }

  // Always rebuild cache for cat_height/mirror/etc changes (even if no
  // surface changes). Skip if we already rebuilt above.
  if (!needs_full_recreate && !needs_buffer_recreate) {
    pthread_mutex_lock(&anim_lock);
    animation_invalidate_cache();
    int cat_h = phys_dim(config->cat_height);
    int cat_w = (cat_h * CAT_IMAGE_WIDTH) / CAT_IMAGE_HEIGHT;
    animation_cache_frames(cat_w, cat_h, config->mirror_x, config->mirror_y,
                           config->enable_antialiasing);
    pthread_mutex_unlock(&anim_lock);
  }

  free(old_output_name);
  old_output_name = NULL;

  applied_width = config->screen_width;
  applied_height = config->overlay_height;
  applied_layer = config->layer;
  applied_position = config->overlay_position;
  free(applied_output_name);
  applied_output_name =
      config->output_name ? strdup(config->output_name) : NULL;

  if (atomic_load(&configured)) {
    draw_bar();
  }
}

void wayland_cleanup(void) {
  bongocat_log_info("Cleaning up Wayland resources");

  fullscreen_cleanup();

  // First destroy xdg_output objects
  for (size_t i = 0; i < MAX_OUTPUTS; ++i) {
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
  for (size_t i = 0; i < MAX_OUTPUTS; ++i) {
    if (outputs[i].wl_output) {
      bongocat_log_debug("Destroying wl_output %zu", i);
      wl_output_destroy(outputs[i].wl_output);
      outputs[i].wl_output = NULL;
    }
  }

  output_count = 0;

  retire_active_buffers();
  while (retired_buffers) {
    buffer_slot_t *slot = retired_buffers;
    retired_buffers = slot->next;
    destroy_buffer_slot(slot);
  }

  if (layer_surface) {
    zwlr_layer_surface_v1_destroy(layer_surface);
    layer_surface = NULL;
  }

  // Destroy fractional-scale and viewport receivers before the surface they
  // reference.
  if (fractional_scale_obj) {
    wp_fractional_scale_v1_destroy(fractional_scale_obj);
    fractional_scale_obj = NULL;
  }
  if (viewport) {
    wp_viewport_destroy(viewport);
    viewport = NULL;
  }

  if (surface) {
    wl_surface_destroy(surface);
    surface = NULL;
  }

  if (fractional_scale_mgr) {
    wp_fractional_scale_manager_v1_destroy(fractional_scale_mgr);
    fractional_scale_mgr = NULL;
  }
  if (viewporter) {
    wp_viewporter_destroy(viewporter);
    viewporter = NULL;
  }

  // Note: output is just a reference to one of the outputs[] entries
  // It will be destroyed when we destroy the outputs[] array above
  output = NULL;

  if (layer_shell) {
    if (layer_shell_version >= ZWLR_LAYER_SHELL_V1_DESTROY_SINCE_VERSION)
      zwlr_layer_shell_v1_destroy(layer_shell);
    else
      wl_proxy_destroy((struct wl_proxy *)layer_shell);
    layer_shell = NULL;
    layer_shell_version = 0;
  }

  if (shm) {
    wl_shm_destroy(shm);
    shm = NULL;
  }

  if (compositor) {
    wl_compositor_destroy(compositor);
    compositor = NULL;
  }

  if (global_registry) {
    wl_registry_destroy(global_registry);
    global_registry = NULL;
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
  bound_screen_name = NULL;
  current_output_info = NULL;
  free(applied_output_name);
  applied_output_name = NULL;
  applied_width = 0;
  applied_height = 0;
  applied_layer = LAYER_TOP;
  applied_position = POSITION_BOTTOM;
  tick_callback_fn = NULL;
  current_scale_120 = 120;
  atomic_store(&pending_scale_120, 120);
  atomic_store(&scale_change_pending, false);
  atomic_store(&output_change_pending, false);
  atomic_store(&redraw_pending, false);
  physical_buffer_w = 0;
  physical_buffer_h = 0;
  memset(&outputs, 0, sizeof(output_ref_t) * MAX_OUTPUTS);

  bongocat_log_debug("Wayland cleanup complete");
}
