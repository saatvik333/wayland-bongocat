#define _GNU_SOURCE
#include "platform/wayland.h"

#include "core/bongocat.h"
#include "graphics/animation.h"
#include "graphics/paw_frame.h"
#include "platform/fullscreen.h"
#include "platform/niri.h"
#include "platform/outputs.h"
#include "platform/scale.h"
#include "platform/shm_buffer.h"
#include "utils/clock.h"
#include "utils/error.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>

#ifdef __GNUC__
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#endif
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-foreign-toplevel-management-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "zwlr-layer-shell-v1-client-protocol.h"
#ifdef __GNUC__
#  pragma GCC diagnostic pop
#endif

static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zxdg_output_manager_v1 *xdg_manager;
static struct wp_viewporter *viewporter;
static struct wp_fractional_scale_manager_v1 *fractional_manager;

static const config_t *config;
static bool hidden, paused, sleeping;
static unsigned pending_paws;
static int64_t pending_time;
static bool reconcile_pending;
static bool flush_blocked;
static uint32_t automatic_id;  // Output followed in automatic mode
// Outputs whose overlay the compositor closed or that failed to render; not
// retried until outputs or the configuration change.
static uint32_t failed_ids[MAX_OUTPUTS];

typedef struct {
  unsigned generation;
  bool visible;
} buffer_content_t;

// One overlay per output slot, at the same index as outputs[].
typedef struct {
  uint32_t output_id;
  struct wl_output *output;
  struct wl_surface *surface;
  struct zwlr_layer_surface_v1 *layer;
  struct wp_viewport *viewport;
  struct wp_fractional_scale_v1 *fractional;
  shm_buffer_t *buffers[2];
  buffer_content_t content[2];
  appearance_t look;
  anim_state_t anim;
  frame_cache_t cache;
  uint32_t scale;     // In 120ths
  int width, height;  // Logical size from configure
  int buffer_width, buffer_height;
  int64_t next_draw;  // Frame changes are limited to fps
  unsigned generation;
  unsigned shown_generation;  // Generation of the committed buffer
  int shown_frame;
  bool shown_visible;
  bool configured, resize;
} overlay_t;

static overlay_t overlays[MAX_OUTPUTS];

// =============================================================================
// RENDERING
// =============================================================================

static int clamp_int(int64_t value) {
  if (value < INT_MIN) {
    return INT_MIN;
  }
  return value > INT_MAX ? INT_MAX : (int)value;
}

static void fill(shm_buffer_t *buffer, int width, int x0, int y0, int x1,
                 int y1, uint32_t color) {
  uint32_t *pixels = (uint32_t *)buffer->pixels;
  for (int y = y0; y < y1; y++) {
    for (int x = x0; x < x1; x++) {
      pixels[((size_t)y * width) + x] = color;
    }
  }
}

static int clip(int value, int limit) {
  if (value < 0) {
    return 0;
  }
  return value > limit ? limit : value;
}

// Draws into a free buffer. Unless the buffer is stale, only the cat
// rectangle is repainted and damaged: the bar around it never changes.
static void draw(overlay_t *overlay, bool visible) {
  int slot = -1;
  for (int i = 0; i < 2 && slot < 0; i++) {
    slot = overlay->buffers[i]->busy ? -1 : i;
  }
  if (slot < 0) {
    return;  // Retried once the compositor releases a buffer.
  }
  shm_buffer_t *buffer = overlay->buffers[slot];
  buffer_content_t *content = &overlay->content[slot];
  const cached_frame_t *frame = &overlay->cache.frames[overlay->anim.frame];
  const appearance_t *look = &overlay->look;
  int width = overlay->buffer_width;
  int height = overlay->buffer_height;
  int64_t x = scale_offset_120(look->cat_x_offset, overlay->scale);
  int64_t y = scale_offset_120(look->cat_y_offset, overlay->scale) +
              (((int64_t)height - frame->height) / 2);
  if (look->cat_align == ALIGN_CENTER) {
    x += ((int64_t)width - frame->width) / 2;
  } else if (look->cat_align == ALIGN_RIGHT) {
    x = (int64_t)width - frame->width - x;
  }
  int x0 = clip(clamp_int(x), width);
  int y0 = clip(clamp_int(y), height);
  int x1 = clip(clamp_int(x + frame->width), width);
  int y1 = clip(clamp_int(y + frame->height), height);
  uint32_t background = visible ? (uint32_t)look->overlay_opacity << 24 : 0;
  // Damage is relative to the buffer shown now, which may be the other one.
  bool full = content->generation != overlay->generation ||
              content->visible != visible ||
              overlay->shown_generation != overlay->generation ||
              overlay->shown_visible != visible;
  if (full) {
    fill(buffer, width, 0, 0, width, height, background);
  } else {
    fill(buffer, width, x0, y0, x1, y1, background);
  }
  if (visible) {
    blit_frame(buffer->pixels, width, height, frame, clamp_int(x),
               clamp_int(y));
  }
  buffer->busy = true;
  wl_surface_attach(overlay->surface, buffer->object, 0, 0);
  if (full) {
    wl_surface_damage_buffer(overlay->surface, 0, 0, width, height);
  } else if (x1 > x0 && y1 > y0) {
    wl_surface_damage_buffer(overlay->surface, x0, y0, x1 - x0, y1 - y0);
  }
  wl_surface_commit(overlay->surface);
  *content = (buffer_content_t){overlay->generation, visible};
  overlay->shown_generation = overlay->generation;
  overlay->shown_frame = overlay->anim.frame;
  overlay->shown_visible = visible;
}

static bool resize_buffers(overlay_t *overlay) {
  int width = scale_size_120(overlay->width, overlay->scale);
  int height = scale_size_120(overlay->height, overlay->scale);
  shm_buffer_t *a = shm_buffer_create(shm, width, height);
  shm_buffer_t *b = a ? shm_buffer_create(shm, width, height) : NULL;
  if (!b) {
    shm_buffer_retire(a);
    return false;
  }
  for (size_t i = 0; i < 2; i++) {
    shm_buffer_retire(overlay->buffers[i]);
  }
  overlay->buffers[0] = a;
  overlay->buffers[1] = b;
  overlay->buffer_width = width;
  overlay->buffer_height = height;
  if (overlay->viewport) {
    wl_surface_set_buffer_scale(overlay->surface, 1);
    wp_viewport_set_destination(overlay->viewport, overlay->width,
                                overlay->height);
  } else {
    wl_surface_set_buffer_scale(overlay->surface,
                                (int32_t)(overlay->scale / 120));
  }
  overlay->resize = false;
  overlay->generation++;
  return true;
}

// =============================================================================
// OVERLAY LIFECYCLE
// =============================================================================

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
  frame_cache_free(&overlay->cache);
  *overlay = (overlay_t){0};
}

static void mark_failed(uint32_t id) {
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    if (!failed_ids[i] || failed_ids[i] == id) {
      failed_ids[i] = id;
      return;
    }
  }
}

static bool failed(uint32_t id) {
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    if (failed_ids[i] == id) {
      return true;
    }
  }
  return false;
}

static void configure(void *data, struct zwlr_layer_surface_v1 *layer,
                      uint32_t serial, uint32_t width, uint32_t height) {
  overlay_t *overlay = data;
  zwlr_layer_surface_v1_ack_configure(layer, serial);
  int w = width && width <= INT_MAX ? (int)width : overlay->width;
  int h =
      height && height <= INT_MAX ? (int)height : overlay->look.overlay_height;
  if (overlay->width != w || overlay->height != h) {
    overlay->width = w;
    overlay->height = h;
    overlay->resize = true;
  }
  overlay->configured = true;
}

static void closed(void *data, struct zwlr_layer_surface_v1 *layer) {
  (void)layer;
  overlay_t *overlay = data;
  mark_failed(overlay->output_id);
  reconcile_pending = true;
}

static const struct zwlr_layer_surface_v1_listener LAYER_LISTENER = {
    .configure = configure,
    .closed = closed,
};

static void preferred_scale(void *data, struct wp_fractional_scale_v1 *object,
                            uint32_t scale) {
  (void)object;
  overlay_t *overlay = data;
  if (scale && scale != overlay->scale) {
    overlay->scale = scale;
    overlay->resize = true;
  }
}

static const struct wp_fractional_scale_v1_listener FRACTIONAL_LISTENER = {
    .preferred_scale = preferred_scale,
};

static uint32_t layer_value(int layer) {
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

static uint32_t integer_scale(const output_ref_t *ref) {
  return ref->scale > 0 && ref->scale <= INT_MAX / 120
             ? (uint32_t)ref->scale * 120
             : 120;
}

// Applies look to the layer surface; the caller commits.
static void apply_look(overlay_t *overlay, const appearance_t *look) {
  overlay->look = *look;
  overlay->generation++;
  uint32_t anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                    ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
                    (look->overlay_position == POSITION_TOP
                         ? ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
                         : ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
  zwlr_layer_surface_v1_set_anchor(overlay->layer, anchor);
  zwlr_layer_surface_v1_set_size(overlay->layer, 0,
                                 (uint32_t)look->overlay_height);
  zwlr_layer_surface_v1_set_exclusive_zone(overlay->layer, -1);
  zwlr_layer_surface_v1_set_keyboard_interactivity(overlay->layer, 0);
}

static void create(overlay_t *overlay, const output_ref_t *ref,
                   const appearance_t *look, const anim_state_t *anim) {
  *overlay = (overlay_t){
      .output_id = ref->id,
      .output = ref->wl_output,
      .scale = integer_scale(ref),
      .width = ref->width,
      .height = look->overlay_height,
      .shown_frame = -1,
  };
  if (anim) {
    overlay->anim = *anim;
  } else {
    anim_reset(&overlay->anim, config, monotonic_us());
  }
  overlay->surface = wl_compositor_create_surface(compositor);
  overlay->layer = zwlr_layer_shell_v1_get_layer_surface(
      layer_shell, overlay->surface, ref->wl_output, layer_value(look->layer),
      "bongocat-overlay");
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
  apply_look(overlay, look);
  overlay->resize = true;
  wl_surface_commit(overlay->surface);
}

static void update(overlay_t *overlay, const output_ref_t *ref,
                   const appearance_t *look) {
  if (!overlay->fractional && integer_scale(ref) != overlay->scale) {
    overlay->scale = integer_scale(ref);
    overlay->resize = true;
  }
  if (memcmp(look, &overlay->look, sizeof(*look)) == 0) {
    return;
  }
  if (look->overlay_height != overlay->look.overlay_height) {
    overlay->height = look->overlay_height;
    overlay->resize = true;
  }
  apply_look(overlay, look);
  wl_surface_commit(overlay->surface);
}

// =============================================================================
// OUTPUT SELECTION
// =============================================================================

static bool usable(const output_ref_t *ref) {
  return ref->wl_output && ref->width > 0 && !failed(ref->id);
}

static output_ref_t *output_named(const char *name) {
  for (size_t i = 0; name && i < MAX_OUTPUTS; i++) {
    if (usable(&outputs[i]) && strcmp(outputs[i].name, name) == 0) {
      return &outputs[i];
    }
  }
  return NULL;
}

static bool follow_focus(void) {
  return !config->num_monitors && config->follow_active_monitor;
}

// Automatic mode without following: the first usable output. Following: the
// focused output when known, else the current one, else the first.
static output_ref_t *automatic_output(void) {
  output_ref_t *first = NULL;
  output_ref_t *current = NULL;
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    if (usable(&outputs[i])) {
      first = first ? first : &outputs[i];
      current = outputs[i].id == automatic_id ? &outputs[i] : current;
    }
  }
  if (!follow_focus()) {
    return first;
  }
  // niri also reports focused outputs whose workspace has no window.
  output_ref_t *focused = output_named(niri_focused_output());
  struct wl_output *object =
      fullscreen_focused_output(current ? current->wl_output : NULL);
  for (size_t i = 0; !focused && object && i < MAX_OUTPUTS; i++) {
    focused = outputs[i].wl_output == object && usable(&outputs[i])
                  ? &outputs[i]
                  : NULL;
  }
  if (focused) {
    return focused;
  }
  return current ? current : first;
}

static bool selected(const output_ref_t *ref) {
  if (!usable(ref)) {
    return false;
  }
  if (!config->num_monitors) {
    return ref->id == automatic_id;
  }
  for (int i = 0; i < config->num_monitors; i++) {
    if (strcmp(ref->name, config->monitors[i]) == 0) {
      return true;
    }
  }
  return false;
}

static void reconcile(void) {
  reconcile_pending = false;
  bool automatic = !config->num_monitors;
  if (automatic) {
    output_ref_t *target = automatic_output();
    uint32_t id = target ? target->id : 0;
    if (id != automatic_id) {
      bongocat_log_debug("Automatic monitor: %s",
                         target ? target->name : "unavailable");
    }
    automatic_id = id;
  }
  // Retire overlays first; a followed overlay hands its timers on.
  anim_state_t carried;
  bool carry = false;
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    overlay_t *overlay = &overlays[i];
    if (overlay->surface &&
        (!selected(&outputs[i]) || overlay->output_id != outputs[i].id)) {
      if (automatic && !carry) {
        carried = overlay->anim;
        carry = true;
      }
      teardown(overlay);
    }
  }
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    overlay_t *overlay = &overlays[i];
    if (!selected(&outputs[i])) {
      continue;
    }
    appearance_t look = config_appearance(config, outputs[i].name);
    if (overlay->surface && look.layer != overlay->look.layer) {
      // The layer is fixed at creation.
      carried = overlay->anim;
      carry = true;
      teardown(overlay);
    }
    if (overlay->surface) {
      update(overlay, &outputs[i], &look);
    } else {
      create(overlay, &outputs[i], &look, carry ? &carried : NULL);
      carry = false;
    }
  }
}

// =============================================================================
// EVENT LOOP
// =============================================================================

static bool overlay_visible(const overlay_t *overlay) {
  bool covered = overlay->look.layer != LAYER_OVERLAY &&
                 !overlay->look.disable_fullscreen_hide &&
                 fullscreen_on(overlay->output);
  return !hidden && !covered;
}

static void earliest(int64_t *deadline, int64_t candidate) {
  if (candidate && (!*deadline || candidate < *deadline)) {
    *deadline = candidate;
  }
}

// Steps the animation and draws when the visible content changed. Returns
// false when the overlay cannot render and was retired.
static bool render(overlay_t *overlay, unsigned paws, const anim_step_t *step,
                   int64_t *deadline) {
  if (!overlay->configured) {
    return true;
  }
  if (overlay->resize && !resize_buffers(overlay)) {
    bongocat_log_error("Cannot allocate %dx%d buffers for the overlay",
                       scale_size_120(overlay->width, overlay->scale),
                       scale_size_120(overlay->height, overlay->scale));
    return false;
  }
  // The cache only changes with scale or appearance, which already advanced
  // the generation.
  int cat_height = scale_size_120(overlay->look.cat_height, overlay->scale);
  int cat_width =
      (int)(((int64_t)cat_height * CAT_IMAGE_WIDTH) / CAT_IMAGE_HEIGHT);
  if (!frame_cache_update(&overlay->cache, cat_width, cat_height,
                          overlay->look.mirror_x, overlay->look.mirror_y)) {
    bongocat_log_error("Cannot rasterize the cat at %dx%d", cat_width,
                       cat_height);
    return false;
  }
  if (config->enable_hand_mapping) {
    paws = paw_apply_mirror(paws, overlay->look.mirror_x);
  }
  int64_t animation = anim_step(&overlay->anim, config, paws, step);
  bool visible = overlay_visible(overlay);
  if (!visible) {
    if (overlay->shown_visible ||
        overlay->shown_generation != overlay->generation) {
      draw(overlay, false);
    }
    return true;
  }
  earliest(deadline, animation);
  bool stale = overlay->shown_generation != overlay->generation ||
               !overlay->shown_visible;
  if (!stale && overlay->anim.frame == overlay->shown_frame) {
    return true;
  }
  // Frame changes, but not visibility or appearance changes, respect fps.
  if (!stale && !step->paused && step->now < overlay->next_draw) {
    earliest(deadline, overlay->next_draw);
    return true;
  }
  draw(overlay, true);
  overlay->next_draw = step->now + (1000000 / config->fps);
  return true;
}

static void update_focus(void) {
  bool follow = follow_focus();
  bool hiding = false;
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    hiding |= overlays[i].surface && overlays[i].look.layer != LAYER_OVERLAY &&
              !overlays[i].look.disable_fullscreen_hide;
  }
  // niri tells which output an empty focused workspace is on.
  niri_set_enabled(follow || hiding);
  bool focus_changed = fullscreen_take_changed();
  if (niri_take_changed()) {
    focus_changed = true;
    output_ref_t *ref = output_named(niri_focused_output());
    if (ref && niri_focused_empty()) {
      fullscreen_forget(ref->wl_output);
    }
  }
  if (focus_changed && follow) {
    output_ref_t *target = automatic_output();
    reconcile_pending |= (target ? target->id : 0) != automatic_id;
  }
}

bongocat_error_t wayland_prepare(struct pollfd *pfd, int64_t *deadline) {
  *deadline = 0;
  if (wl_display_dispatch_pending(display) < 0) {
    return BONGOCAT_ERROR_WAYLAND;
  }
  if (flush_blocked && wl_display_flush(display) >= 0) {
    flush_blocked = false;
  }
  fullscreen_settle();
  if (outputs_take_changed()) {
    memset(failed_ids, 0, sizeof(failed_ids));
    reconcile_pending = true;
  }
  update_focus();
  // While the compositor is not reading, queue nothing new.
  if (!flush_blocked) {
    if (reconcile_pending) {
      reconcile();
    }
    unsigned paws = pending_paws;
    if (paws && !config->enable_hand_mapping) {
      paws = anim_random_paw();  // One choice for every overlay
    }
    anim_step_t step = {.now = monotonic_us(),
                        .input_time = pending_time,
                        .paused = paused,
                        .sleeping = sleeping};
    pending_paws = 0;
    for (size_t i = 0; i < MAX_OUTPUTS; i++) {
      if (overlays[i].surface && !render(&overlays[i], paws, &step, deadline)) {
        mark_failed(overlays[i].output_id);
        teardown(&overlays[i]);
        reconcile_pending = true;
      }
    }
  }
  while (wl_display_prepare_read(display) != 0) {
    if (wl_display_dispatch_pending(display) < 0) {
      return BONGOCAT_ERROR_WAYLAND;
    }
  }
  *pfd = (struct pollfd){.fd = wl_display_get_fd(display), .events = POLLIN};
  if (wl_display_flush(display) < 0) {
    if (errno != EAGAIN) {
      wl_display_cancel_read(display);
      return BONGOCAT_ERROR_WAYLAND;
    }
    flush_blocked = true;
    pfd->events |= POLLOUT;
  }
  if (reconcile_pending && !flush_blocked) {
    *deadline = monotonic_us();
  }
  return BONGOCAT_SUCCESS;
}

bongocat_error_t wayland_dispatch(const struct pollfd *pfd) {
  if (pfd->revents & POLLIN) {
    if (wl_display_read_events(display) < 0) {
      return BONGOCAT_ERROR_WAYLAND;
    }
  } else {
    wl_display_cancel_read(display);
  }
  if (pfd->revents & (POLLERR | POLLHUP | POLLNVAL)) {
    return BONGOCAT_ERROR_WAYLAND;
  }
  return wl_display_dispatch_pending(display) < 0 ? BONGOCAT_ERROR_WAYLAND
                                                  : BONGOCAT_SUCCESS;
}

void wayland_set_hidden(bool value) {
  hidden = value;
}

void wayland_set_sleeping(bool value) {
  sleeping = value;
}

void wayland_set_paused(bool value) {
  paused = value;
  pending_paws = 0;
  int64_t now = monotonic_us();
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    anim_reset(&overlays[i].anim, config, now);
  }
}

void wayland_add_paws(unsigned paws, int64_t time) {
  if (!paused && paws) {
    pending_paws |= paws;
    pending_time = time;
  }
}

// =============================================================================
// CONNECTION
// =============================================================================

static void global(void *data, struct wl_registry *object, uint32_t id,
                   const char *interface, uint32_t version) {
  (void)data;
  if (strcmp(interface, wl_compositor_interface.name) == 0 && version >= 4) {
    compositor = wl_registry_bind(object, id, &wl_compositor_interface, 4);
  } else if (strcmp(interface, wl_shm_interface.name) == 0) {
    shm = wl_registry_bind(object, id, &wl_shm_interface, 1);
  } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
    layer_shell = wl_registry_bind(object, id, &zwlr_layer_shell_v1_interface,
                                   version < 4 ? version : 4);
  } else if (strcmp(interface, wl_output_interface.name) == 0) {
    outputs_add(object, id, version);
  } else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0 &&
             version >= 2) {
    xdg_manager =
        wl_registry_bind(object, id, &zxdg_output_manager_v1_interface,
                         version < 3 ? version : 3);
    outputs_set_manager(xdg_manager);
  } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
    viewporter = wl_registry_bind(object, id, &wp_viewporter_interface, 1);
  } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) ==
             0) {
    fractional_manager = wl_registry_bind(
        object, id, &wp_fractional_scale_manager_v1_interface, 1);
  } else if (strcmp(interface,
                    zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
    fullscreen_init(wl_registry_bind(
        object, id, &zwlr_foreign_toplevel_manager_v1_interface,
        version < 3 ? version : 3));
  }
}

static void global_remove(void *data, struct wl_registry *object, uint32_t id) {
  (void)data;
  (void)object;
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    if (outputs[i].wl_output && outputs[i].id == id) {
      if (overlays[i].surface) {
        teardown(&overlays[i]);
      }
      fullscreen_output_removed(outputs[i].wl_output);
      outputs_remove(id);
      reconcile_pending = true;
    }
  }
}

static const struct wl_registry_listener REGISTRY_LISTENER = {
    .global = global,
    .global_remove = global_remove,
};

static bool connect_display(void) {
  display = wl_display_connect(NULL);
  if (!display) {
    return false;
  }
  registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &REGISTRY_LISTENER, NULL);
  // The second roundtrip delivers output and xdg-output events.
  for (int pass = 0; pass < 2; pass++) {
    if (wl_display_roundtrip(display) < 0) {
      return false;
    }
  }
  return true;
}

int wayland_list_monitors(bool doctor) {
  if (!connect_display()) {
    fprintf(stderr, "Wayland: cannot connect; check WAYLAND_DISPLAY and "
                    "XDG_RUNTIME_DIR\n");
    wayland_cleanup();
    return 1;
  }
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    if (outputs[i].wl_output) {
      printf("%s: %dx%d scale=%d\n", outputs[i].name, outputs[i].width,
             outputs[i].height, outputs[i].scale);
    }
  }
  int failure = !compositor || !shm || !layer_shell;
  if (doctor) {
    printf("Protocols: compositor-v4=%s shm=%s layer-shell=%s "
           "viewporter=%s fractional-scale=%s foreign-toplevel=%s\n",
           compositor ? "yes" : "missing", shm ? "yes" : "missing",
           layer_shell ? "yes" : "missing", viewporter ? "yes" : "no",
           fractional_manager ? "yes" : "no",
           fullscreen_available() ? "yes" : "no");
  }
  wayland_cleanup();
  return failure;
}

bongocat_error_t wayland_init(const config_t *settings) {
  config = settings;
  if (!connect_display() || !compositor || !shm || !layer_shell) {
    bongocat_log_error(
        "Wayland requires wl_compositor v4, wl_shm and layer-shell");
    wayland_cleanup();
    return BONGOCAT_ERROR_WAYLAND;
  }
  reconcile_pending = true;
  return BONGOCAT_SUCCESS;
}

void wayland_update_config(const config_t *settings) {
  config = settings;
  memset(failed_ids, 0, sizeof(failed_ids));
  reconcile_pending = true;
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
  automatic_id = 0;
  reconcile_pending = flush_blocked = false;
  memset(failed_ids, 0, sizeof(failed_ids));
}
