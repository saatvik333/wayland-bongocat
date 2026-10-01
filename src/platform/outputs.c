#include "platform/outputs.h"

#include "core/bongocat.h"
#include "platform/scale.h"
#include "xdg-output-unstable-v1-client-protocol.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <wayland-client-protocol.h>

output_ref_t outputs[MAX_OUTPUTS];
size_t output_count;
static struct zxdg_output_manager_v1 *output_manager;
static bool changed;

static void update(output_ref_t *ref) {
  output_logical_size(ref->raw_width, ref->raw_height, ref->transform,
                      ref->wl_scale, ref->width, ref->height,
                      &ref->screen_width, &ref->screen_height);
  changed = true;
}

// Wayland requires this callback signature, including all ten parameters.
// NOLINTNEXTLINE(readability-function-size)
static void geometry(void *data, struct wl_output *object, int32_t x, int32_t y,
                     int32_t pw, int32_t ph, int32_t subpixel, const char *make,
                     const char *model, int32_t transform) {
  (void)object;
  (void)pw;
  (void)ph;
  (void)subpixel;
  (void)make;
  (void)model;
  output_ref_t *ref = data;
  ref->x = x;
  ref->y = y;
  ref->transform = transform;
  update(ref);
}
static void mode(void *data, struct wl_output *object, uint32_t flags,
                 int32_t width, int32_t height, int32_t refresh) {
  (void)object;
  (void)refresh;
  if (!(flags & WL_OUTPUT_MODE_CURRENT)) {
    return;
  }
  output_ref_t *ref = data;
  ref->raw_width = width;
  ref->raw_height = height;
  update(ref);
}
static void done(void *data, struct wl_output *object) {
  (void)object;
  update(data);
}
static void scale(void *data, struct wl_output *object, int32_t value) {
  (void)object;
  output_ref_t *ref = data;
  ref->wl_scale = value > 0 ? value : 1;
  update(ref);
}
static void name(void *data, struct wl_output *object, const char *value) {
  (void)object;
  output_ref_t *ref = data;
  snprintf(ref->name_str, sizeof(ref->name_str), "%s", value);
  ref->name_received = true;
  changed = true;
}
static void description(void *data, struct wl_output *object,
                        const char *value) {
  (void)data;
  (void)object;
  (void)value;
}
static const struct wl_output_listener LISTENER = {.geometry = geometry,
                                                   .mode = mode,
                                                   .done = done,
                                                   .scale = scale,
                                                   .name = name,
                                                   .description = description};

static void xdg_position(void *data, struct zxdg_output_v1 *object, int32_t x,
                         int32_t y) {
  (void)object;
  output_ref_t *ref = data;
  ref->x = x;
  ref->y = y;
  changed = true;
}
static void xdg_size(void *data, struct zxdg_output_v1 *object, int32_t width,
                     int32_t height) {
  (void)object;
  output_ref_t *ref = data;
  ref->width = width;
  ref->height = height;
  update(ref);
}
static void xdg_done(void *data, struct zxdg_output_v1 *object) {
  (void)object;
  update(data);
}
static void xdg_name(void *data, struct zxdg_output_v1 *object,
                     const char *value) {
  (void)object;
  name(data, NULL, value);
}
static void xdg_description(void *data, struct zxdg_output_v1 *object,
                            const char *value) {
  (void)data;
  (void)object;
  (void)value;
}
static const struct zxdg_output_v1_listener XDG_LISTENER = {
    .logical_position = xdg_position,
    .logical_size = xdg_size,
    .done = xdg_done,
    .name = xdg_name,
    .description = xdg_description};

static void attach_xdg(output_ref_t *ref) {
  if (output_manager && ref->wl_output && !ref->xdg_output) {
    ref->xdg_output =
        zxdg_output_manager_v1_get_xdg_output(output_manager, ref->wl_output);
    zxdg_output_v1_add_listener(ref->xdg_output, &XDG_LISTENER, ref);
  }
}
void outputs_set_manager(struct zxdg_output_manager_v1 *manager) {
  output_manager = manager;
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    attach_xdg(&outputs[i]);
  }
}
void outputs_add(struct wl_registry *registry, uint32_t id, uint32_t version) {
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    output_ref_t *ref = &outputs[i];
    if (ref->wl_output) {
      continue;
    }
    *ref = (output_ref_t){.name = id, .wl_scale = 1, .hypr_id = -1};
    ref->wl_output = wl_registry_bind(registry, id, &wl_output_interface,
                                      version < 4 ? version : 4);
    wl_output_add_listener(ref->wl_output, &LISTENER, ref);
    attach_xdg(ref);
    output_count++;
    changed = true;
    return;
  }
}
void outputs_remove(uint32_t id) {
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    output_ref_t *ref = &outputs[i];
    if (!ref->wl_output || ref->name != id) {
      continue;
    }
    if (ref->xdg_output) {
      zxdg_output_v1_destroy(ref->xdg_output);
    }
    if (wl_output_get_version(ref->wl_output) >= 3) {
      wl_output_release(ref->wl_output);
    } else {
      wl_output_destroy(ref->wl_output);
    }
    *ref = (output_ref_t){0};
    output_count--;
    changed = true;
    return;
  }
}
void outputs_cleanup(void) {
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    if (outputs[i].wl_output) {
      outputs_remove(outputs[i].name);
    }
  }
  output_manager = NULL;
}
bool outputs_take_changed(void) {
  bool result = changed;
  changed = false;
  return result;
}
