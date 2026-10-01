#define _GNU_SOURCE
#include "platform/wayland.h"

#include "config/config.h"
#include "core/bongocat.h"
#include "utils/error.h"
#include "zwlr-layer-shell-v1-client-protocol.h"

#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/poll.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

#ifdef __GNUC__
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#endif
#include "fractional-scale-v1-client-protocol.h"
#include "graphics/animation.h"
#include "platform/fullscreen.h"
#include "platform/input.h"
#include "platform/outputs.h"
#include "platform/scale.h"
#include "platform/shm_buffer.h"
#include "viewporter-client-protocol.h"
#include "wlr-foreign-toplevel-management-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#ifdef __GNUC__
#  pragma GCC diagnostic pop
#endif

#include <limits.h>

struct wl_display *display;
struct wl_compositor *compositor;
struct wl_shm *shm;
struct zwlr_layer_shell_v1 *layer_shell;
struct wl_output *output;
struct wl_surface *surface;
struct zwlr_layer_surface_v1 *layer_surface;
atomic_bool configured;
atomic_bool fullscreen_detected;
static struct wl_registry *registry;
static struct zxdg_output_manager_v1 *xdg_manager;
static struct wp_viewporter *viewporter;
static struct wp_fractional_scale_manager_v1 *fractional_manager;
static config_t *global_config;
static void (*tick_callback)(void);
static int (*runtime_fds)(int *, size_t);
static int (*runtime_timeout)(void);
static bool hidden;
static bool reconcile_pending;

typedef struct {
  uint32_t output_id;
  struct wl_output *output;
  char name[128];
  config_t config;
  struct wl_surface *surface;
  struct zwlr_layer_surface_v1 *layer;
  struct wp_viewport *viewport;
  struct wp_fractional_scale_v1 *fractional;
  shm_buffer_t *buffers[2];
  void *animation;
  uint32_t scale;
  int width, height, physical_width, physical_height;
  bool configured, redraw, resize, closed;
} overlay_t;
static overlay_t overlays[MAX_OUTPUTS];
static overlay_t *active;

static void activate(overlay_t *overlay) {
  active = overlay;
  output = overlay->output;
  surface = overlay->surface;
  layer_surface = overlay->layer;
  atomic_store(&configured, overlay->configured);
  animation_overlay_activate(overlay->animation, &overlay->config);
  fullscreen_recompute();
}
int wayland_phys_dim(int logical) {
  return scale_size_120(logical, active ? active->scale : 120);
}
static int clamp_offset(int64_t value) {
  if (value < INT_MIN) {
    return INT_MIN;
  }
  if (value > INT_MAX) {
    return INT_MAX;
  }
  return (int)value;
}
void draw_bar(void) {
  overlay_t *overlay = active;
  if (!overlay || !overlay->configured || overlay->resize) {
    return;
  }
  shm_buffer_t *buffer = NULL;
  for (size_t i = 0; i < 2; i++) {
    if (overlay->buffers[i] && !overlay->buffers[i]->busy) {
      buffer = overlay->buffers[i];
      break;
    }
  }
  if (!buffer) {
    overlay->redraw = true;
    return;
  }
  config_t *config = &overlay->config;
  bool invisible = (hidden || (config->layer != LAYER_OVERLAY &&
                               !config->disable_fullscreen_hide &&
                               atomic_load(&fullscreen_detected))) != 0;
  memset(buffer->pixels, 0, buffer->size);
  if (!invisible) {
    uint32_t *pixels = (uint32_t *)buffer->pixels;
    for (size_t i = 0; i < buffer->size / 4; i++) {
      pixels[i] = (uint32_t)config->overlay_opacity << 24;
    }
    cached_frame_t *frame = &anim_cached_frames[anim_index];
    int64_t x = scale_offset_120(config->cat_x_offset, overlay->scale);
    int64_t y = scale_offset_120(config->cat_y_offset, overlay->scale);
    y += ((int64_t)overlay->physical_height - frame->height) / 2;
    if (config->cat_align == ALIGN_CENTER) {
      x += ((int64_t)overlay->physical_width - frame->width) / 2;
    } else if (config->cat_align == ALIGN_RIGHT) {
      x = (int64_t)overlay->physical_width - frame->width - x;
    }
    if (frame->data) {
      blit_cached_frame(buffer->pixels, overlay->physical_width,
                        overlay->physical_height, frame->data, frame->width,
                        frame->height, clamp_offset(x), clamp_offset(y));
    }
  }
  buffer->busy = true;
  wl_surface_attach(overlay->surface, buffer->object, 0, 0);
  wl_surface_damage_buffer(overlay->surface, 0, 0, overlay->physical_width,
                           overlay->physical_height);
  wl_surface_commit(overlay->surface);
  overlay->redraw = false;
}
void wayland_request_redraw(void) {
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    overlays[i].redraw = true;
  }
}
void wayland_request_current_redraw(void) {
  if (active) {
    active->redraw = true;
  }
}
void wayland_set_hidden(bool value) {
  hidden = value;
  wayland_request_redraw();
}
static void teardown(overlay_t *overlay) {
  if (overlay->layer) {
    zwlr_layer_surface_v1_destroy(overlay->layer);
  }
  if (overlay->fractional) {
    wp_fractional_scale_v1_destroy(overlay->fractional);
  }
  if (overlay->viewport) {
    wp_viewport_destroy(overlay->viewport);
  }
  if (overlay->surface) {
    wl_surface_destroy(overlay->surface);
  }
  for (size_t i = 0; i < 2; i++) {
    shm_buffer_retire(overlay->buffers[i]);
  }
  animation_overlay_destroy(overlay->animation);
  if (active == overlay) {
    active = NULL;
    output = NULL;
    surface = NULL;
    layer_surface = NULL;
  }
  *overlay = (overlay_t){0};
}
static void configure(void *data, struct zwlr_layer_surface_v1 *layer,
                      uint32_t serial, uint32_t width, uint32_t height) {
  overlay_t *overlay = data;
  zwlr_layer_surface_v1_ack_configure(layer, serial);
  if (width > INT_MAX || height > INT_MAX) {
    return;
  }
  int w = width ? (int)width : overlay->config.screen_width;
  int h = height ? (int)height : overlay->config.overlay_height;
  if (overlay->width != w || overlay->height != h) {
    overlay->width = w;
    overlay->height = h;
    overlay->resize = true;
  }
  overlay->configured = true;
  overlay->redraw = true;
}
static void closed(void *data, struct zwlr_layer_surface_v1 *layer) {
  (void)layer;
  overlay_t *overlay = data;
  overlay->configured = false;
  overlay->closed = true;
  reconcile_pending = true;
}
static const struct zwlr_layer_surface_v1_listener LAYER_LISTENER = {
    .configure = configure, .closed = closed};
static void preferred_scale(void *data, struct wp_fractional_scale_v1 *object,
                            uint32_t scale) {
  (void)object;
  overlay_t *overlay = data;
  if (scale && scale != overlay->scale) {
    overlay->scale = scale;
    overlay->resize = true;
    overlay->redraw = true;
  }
}
static const struct wp_fractional_scale_v1_listener FRACTIONAL_LISTENER = {
    .preferred_scale = preferred_scale};
static uint32_t layer_value(layer_type_t layer) {
  switch (layer) {
  case LAYER_BACKGROUND:
    return ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
  case LAYER_BOTTOM:
    return ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
  case LAYER_OVERLAY:
    return ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
  default:
    return ZWLR_LAYER_SHELL_V1_LAYER_TOP;
  }
}
static void properties(overlay_t *overlay) {
  uint32_t anchor =
      ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
  anchor |= overlay->config.overlay_position == POSITION_TOP
                ? ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
                : ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
  zwlr_layer_surface_v1_set_anchor(overlay->layer, anchor);
  zwlr_layer_surface_v1_set_size(overlay->layer, 0,
                                 overlay->config.overlay_height);
  zwlr_layer_surface_v1_set_exclusive_zone(overlay->layer, -1);
  zwlr_layer_surface_v1_set_keyboard_interactivity(overlay->layer, 0);
}
static bool create(overlay_t *overlay, output_ref_t *ref) {
  overlay->output_id = ref->name;
  overlay->output = ref->wl_output;
  snprintf(overlay->name, sizeof(overlay->name), "%s", ref->name_str);
  config_for_monitor(global_config, ref->name_str, &overlay->config);
  overlay->config.screen_width = ref->screen_width;
  overlay->scale = ref->wl_scale > 0 && ref->wl_scale <= INT_MAX / 120
                       ? (uint32_t)ref->wl_scale * 120
                       : 120;
  overlay->width = ref->screen_width;
  overlay->height = overlay->config.overlay_height;
  overlay->animation = animation_overlay_create(&overlay->config);
  if (!overlay->animation) {
    teardown(overlay);
    return false;
  }
  overlay->surface = wl_compositor_create_surface(compositor);
  overlay->layer = zwlr_layer_shell_v1_get_layer_surface(
      layer_shell, overlay->surface, ref->wl_output,
      layer_value(overlay->config.layer), "bongocat-overlay");
  zwlr_layer_surface_v1_add_listener(overlay->layer, &LAYER_LISTENER, overlay);
  struct wl_region *region = wl_compositor_create_region(compositor);
  wl_surface_set_input_region(overlay->surface, region);
  wl_region_destroy(region);
  if (viewporter) {
    overlay->viewport =
        wp_viewporter_get_viewport(viewporter, overlay->surface);
    if (fractional_manager) {
      overlay->fractional = wp_fractional_scale_manager_v1_get_fractional_scale(
          fractional_manager, overlay->surface);
      wp_fractional_scale_v1_add_listener(overlay->fractional,
                                          &FRACTIONAL_LISTENER, overlay);
    }
  }
  properties(overlay);
  overlay->resize = true;
  overlay->redraw = true;
  wl_surface_commit(overlay->surface);
  return true;
}
static bool selected(output_ref_t *ref, bool first) {
  if (global_config->output_name && global_config->num_output_names <= 1) {
    return strcmp(ref->name_str, global_config->output_name) == 0;
  }
  if (global_config->num_output_names) {
    for (int i = 0; i < global_config->num_output_names; i++) {
      if (strcmp(ref->name_str, global_config->output_names[i]) == 0) {
        return true;
      }
    }
    return false;
  }
  return first;
}
static void reconcile(void) {
  bool first = true;
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    output_ref_t *ref = &outputs[i];
    overlay_t *overlay = &overlays[i];
    bool wanted =
        (ref->wl_output && ref->screen_width > 0 && selected(ref, first)) != 0;
    if (ref->wl_output && ref->screen_width > 0) {
      first = false;
    }
    if (overlay->surface &&
        (!wanted || overlay->output_id != ref->name || overlay->closed)) {
      teardown(overlay);
    }
    if (!wanted) {
      continue;
    }
    if (!overlay->surface) {
      create(overlay, ref);
      continue;
    }
    config_t effective;
    config_for_monitor(global_config, ref->name_str, &effective);
    effective.screen_width = ref->screen_width;
    if (effective.layer != overlay->config.layer) {
      teardown(overlay);
      create(overlay, ref);
      continue;
    }
    bool size_changed =
        (effective.overlay_height != overlay->config.overlay_height ||
         effective.screen_width != overlay->config.screen_width) != 0;
    overlay->config = effective;
    if (!overlay->fractional) {
      uint32_t scale = ref->wl_scale > 0 && ref->wl_scale <= INT_MAX / 120
                           ? (uint32_t)ref->wl_scale * 120
                           : 120;
      if (scale != overlay->scale) {
        overlay->scale = scale;
        overlay->resize = true;
      }
    }
    if (size_changed) {
      overlay->width = effective.screen_width;
      overlay->height = effective.overlay_height;
      overlay->resize = true;
    }
    properties(overlay);
    overlay->redraw = true;
    wl_surface_commit(overlay->surface);
  }
  reconcile_pending = false;
}
static bool resize_buffers(overlay_t *overlay) {
  int width = scale_size_120(overlay->width, overlay->scale);
  int height = scale_size_120(overlay->height, overlay->scale);
  shm_buffer_t *a = shm_buffer_create(shm, width, height);
  shm_buffer_t *b = a ? shm_buffer_create(shm, width, height) : NULL;
  if (!a || !b) {
    shm_buffer_retire(a);
    shm_buffer_retire(b);
    return false;
  }
  for (size_t i = 0; i < 2; i++) {
    shm_buffer_retire(overlay->buffers[i]);
  }
  overlay->buffers[0] = a;
  overlay->buffers[1] = b;
  overlay->physical_width = width;
  overlay->physical_height = height;
  if (overlay->viewport) {
    wl_surface_set_buffer_scale(overlay->surface, 1);
    wp_viewport_set_destination(overlay->viewport, overlay->width,
                                overlay->height);
  } else {
    {
      wl_surface_set_buffer_scale(overlay->surface,
                                  (int)(overlay->scale / 120));
    }
  }
  overlay->resize = false;
  return true;
}
static void global(void *data, struct wl_registry *object, uint32_t id,
                   const char *interface, uint32_t version) {
  (void)data;
  if (strcmp(interface, wl_compositor_interface.name) == 0 && version >= 4) {
    {
      compositor = wl_registry_bind(object, id, &wl_compositor_interface, 4);
    }
  } else if (strcmp(interface, wl_shm_interface.name) == 0) {
    { shm = wl_registry_bind(object, id, &wl_shm_interface, 1); }
  } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
    {
      layer_shell = wl_registry_bind(object, id, &zwlr_layer_shell_v1_interface,
                                     version < 4 ? version : 4);
    }
  } else if (strcmp(interface, wl_output_interface.name) == 0) {
    { outputs_add(object, id, version); }
  } else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0 &&
             version >= 2) {
    xdg_manager =
        wl_registry_bind(object, id, &zxdg_output_manager_v1_interface,
                         version < 3 ? version : 3);
    outputs_set_manager(xdg_manager);
  } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
    { viewporter = wl_registry_bind(object, id, &wp_viewporter_interface, 1); }
  } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) ==
             0) {
    {
      fractional_manager = wl_registry_bind(
          object, id, &wp_fractional_scale_manager_v1_interface, 1);
    }
  } else if (strcmp(interface,
                    zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
    struct zwlr_foreign_toplevel_manager_v1 *manager = wl_registry_bind(
        object, id, &zwlr_foreign_toplevel_manager_v1_interface,
        version < 3 ? version : 3);
    fullscreen_init(manager);
  }
}
static void removed(void *data, struct wl_registry *object, uint32_t id) {
  (void)data;
  (void)object;
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    if (overlays[i].surface && overlays[i].output_id == id) {
      teardown(&overlays[i]);
    }
  }
  fullscreen_output_removed(id);
  outputs_remove(id);
  reconcile_pending = true;
}
static const struct wl_registry_listener REGISTRY_LISTENER = {
    .global = global, .global_remove = removed};
static int discover(void) {
  display = wl_display_connect(NULL);
  if (!display) {
    return -1;
  }
  registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &REGISTRY_LISTENER, NULL);
  for (int pass = 0; pass < 2; pass++) {
    if (wl_display_roundtrip(display) < 0) {
      return -1;
    }
  }
  if (wl_display_get_error(display) != 0) {
    return -1;
  }
  return 0;
}
int wayland_list_monitors(bool doctor) {
  if (discover() < 0) {
    fprintf(
        stderr,
        "Wayland: cannot connect; check WAYLAND_DISPLAY and XDG_RUNTIME_DIR\n");
    wayland_cleanup();
    return 1;
  }
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    if (outputs[i].wl_output) {
      printf("%s: %dx%d scale=%d\n", outputs[i].name_str,
             outputs[i].screen_width, outputs[i].screen_height,
             outputs[i].wl_scale);
    }
  }
  int failure = !compositor || !shm || !layer_shell;
  if (doctor) {
    printf("Protocols: compositor-v4=%s shm=%s layer-shell=%s "
           "viewporter=%s fractional-scale=%s fullscreen=%s\n",
           compositor ? "yes" : "missing", shm ? "yes" : "missing",
           layer_shell ? "yes" : "missing", viewporter ? "yes" : "no",
           fractional_manager ? "yes" : "no",
           (int)fs_detector_available() ? "yes" : "no");
  }
  wayland_cleanup();
  return failure;
}
bongocat_error_t wayland_init(config_t *config) {
  global_config = config;
  if (discover() < 0 || !compositor || !shm || !layer_shell) {
    bongocat_log_error(
        "Wayland requires wl_compositor v4, wl_shm and layer-shell");
    wayland_cleanup();
    return BONGOCAT_ERROR_WAYLAND;
  }
  reconcile_pending = true;
  return BONGOCAT_SUCCESS;
}
void wayland_update_config(config_t *config) {
  global_config = config;
  reconcile_pending = true;
}
bongocat_error_t wayland_run(const volatile sig_atomic_t *running) {
  bool flush_blocked = false;
  while (*running && display) {
    if (wl_display_dispatch_pending(display) < 0) {
      return BONGOCAT_ERROR_WAYLAND;
    }
    if (tick_callback) {
      tick_callback();
    }
    if (!*running) {
      break;
    }
    if (outputs_take_changed()) {
      reconcile_pending = true;
    }
    if (flush_blocked) {
      if (wl_display_flush(display) >= 0) {
        flush_blocked = false;
      } else if (errno != EAGAIN) {
        return BONGOCAT_ERROR_WAYLAND;
      }
    }
    if (reconcile_pending && !flush_blocked) {
      reconcile();
    }
    input_process_events();
    unsigned paws =
        !flush_blocked && pending_paws ? atomic_exchange(pending_paws, 0) : 0;
    int timeout = -1;
    for (size_t i = 0; !flush_blocked && i < MAX_OUTPUTS; i++) {
      overlay_t *overlay = &overlays[i];
      if (!overlay->surface) {
        continue;
      }
      activate(overlay);
      if (overlay->resize && !resize_buffers(overlay)) {
        return BONGOCAT_ERROR_MEMORY;
      }
      int cat_h = scale_size_120(overlay->config.cat_height, overlay->scale);
      int64_t cat_w = (int64_t)cat_h * CAT_IMAGE_WIDTH / CAT_IMAGE_HEIGHT;
      if (!cat_h || cat_w > INT_MAX) {
        return BONGOCAT_ERROR_MEMORY;
      }
      animation_overlay_cache((int)cat_w, cat_h);
      for (int frame = 0; frame < NUM_FRAMES; frame++) {
        if (!anim_cached_frames[frame].data) {
          return BONGOCAT_ERROR_MEMORY;
        }
      }
      int next = animation_tick(paws);
      if (next >= 0 && (timeout < 0 || next < timeout)) {
        timeout = next;
      }
      if (overlay->redraw) {
        draw_bar();
      }
    }
    int next_runtime = runtime_timeout ? runtime_timeout() : -1;
    if (next_runtime >= 0 && (timeout < 0 || next_runtime < timeout)) {
      timeout = next_runtime;
    }
    struct pollfd fds[8] = {
        {.fd = wl_display_get_fd(display), .events = POLLIN}
    };
    int external[7];
    int count = runtime_fds ? runtime_fds(external, 7) : 0;
    for (int i = 0; i < count; i++) {
      fds[i + 1] = (struct pollfd){.fd = external[i], .events = POLLIN};
    }
    while (wl_display_prepare_read(display) != 0) {
      if (wl_display_dispatch_pending(display) < 0) {
        return BONGOCAT_ERROR_WAYLAND;
      }
    }
    if (wl_display_flush(display) < 0) {
      if (errno != EAGAIN) {
        wl_display_cancel_read(display);
        return BONGOCAT_ERROR_WAYLAND;
      }
      flush_blocked = true;
      fds[0].events |= POLLOUT;
    }
    int result = poll(fds, (nfds_t)count + 1, timeout);
    if (result > 0 && (fds[0].revents & POLLIN)) {
      if (wl_display_read_events(display) < 0) {
        return BONGOCAT_ERROR_WAYLAND;
      }
    } else {
      { wl_display_cancel_read(display); }
    }
    if (result < 0 && errno != EINTR) {
      return BONGOCAT_ERROR_WAYLAND;
    }
    if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
      return BONGOCAT_ERROR_WAYLAND;
    }
  }
  return BONGOCAT_SUCCESS;
}
struct wl_output *wayland_get_current_screen_output(void) {
  return output;
}
void wayland_set_tick_callback(void (*callback)(void)) {
  tick_callback = callback;
}
void wayland_set_runtime_fds(int (*callback)(int *, size_t)) {
  runtime_fds = callback;
}
void wayland_set_runtime_timeout(int (*callback)(void)) {
  runtime_timeout = callback;
}
void wayland_cleanup(void) {
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    teardown(&overlays[i]);
  }
  shm_buffers_cleanup();
  fullscreen_cleanup();
  outputs_cleanup();
  if (xdg_manager) {
    zxdg_output_manager_v1_destroy(xdg_manager);
  }
  if (fractional_manager) {
    wp_fractional_scale_manager_v1_destroy(fractional_manager);
  }
  if (viewporter) {
    wp_viewporter_destroy(viewporter);
  }
  if (layer_shell) {
    zwlr_layer_shell_v1_destroy(layer_shell);
  }
  if (shm) {
    wl_shm_destroy(shm);
  }
  if (compositor) {
    wl_compositor_destroy(compositor);
  }
  if (registry) {
    wl_registry_destroy(registry);
  }
  if (display) {
    wl_display_disconnect(display);
  }
  display = NULL;
  registry = NULL;
  compositor = NULL;
  shm = NULL;
  layer_shell = NULL;
  xdg_manager = NULL;
  fractional_manager = NULL;
  viewporter = NULL;
}
