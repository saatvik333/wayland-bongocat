#include "platform/outputs.h"

#include "platform/scale.h"
#include "xdg-output-unstable-v1-client-protocol.h"

#include <stdio.h>
#include <wayland-client-protocol.h>

output_ref_t outputs[MAX_OUTPUTS];
static struct zxdg_output_manager_v1 *output_manager;
static bool changed;

static void update(output_ref_t *ref) {
  output_logical_size(ref->mode_width, ref->mode_height, ref->transform,
                      ref->scale, ref->xdg_width, ref->xdg_height, &ref->width,
                      &ref->height);
  changed = true;
}

// Wayland requires this callback signature, including all ten parameters.
// NOLINTNEXTLINE(readability-function-size)
static void geometry(void *data, struct wl_output *object, int32_t x, int32_t y,
                     int32_t physical_width, int32_t physical_height,
                     int32_t subpixel, const char *make, const char *model,
                     int32_t transform) {
  (void)object;
  (void)x;
  (void)y;
  (void)physical_width;
  (void)physical_height;
  (void)subpixel;
  (void)make;
  (void)model;
  output_ref_t *ref = data;
  ref->transform = transform;
  update(ref);
}

static void mode(void *data, struct wl_output *object, uint32_t flags,
                 int32_t width, int32_t height, int32_t refresh) {
  (void)object;
  (void)refresh;
  if (flags & WL_OUTPUT_MODE_CURRENT) {
    output_ref_t *ref = data;
    ref->mode_width = width;
    ref->mode_height = height;
    update(ref);
  }
}

static void done(void *data, struct wl_output *object) {
  (void)object;
  update(data);
}

static void scale(void *data, struct wl_output *object, int32_t value) {
  (void)object;
  output_ref_t *ref = data;
  ref->scale = value > 0 ? value : 1;
  update(ref);
}

static void set_name(output_ref_t *ref, const char *value) {
  snprintf(ref->name, sizeof(ref->name), "%s", value);
  changed = true;
}

static void name(void *data, struct wl_output *object, const char *value) {
  (void)object;
  set_name(data, value);
}

static void description(void *data, struct wl_output *object,
                        const char *value) {
  (void)data;
  (void)object;
  (void)value;
}

static const struct wl_output_listener LISTENER = {
    .geometry = geometry,
    .mode = mode,
    .done = done,
    .scale = scale,
    .name = name,
    .description = description,
};

static void xdg_position(void *data, struct zxdg_output_v1 *object, int32_t x,
                         int32_t y) {
  (void)data;
  (void)object;
  (void)x;
  (void)y;
}

static void xdg_size(void *data, struct zxdg_output_v1 *object, int32_t width,
                     int32_t height) {
  (void)object;
  output_ref_t *ref = data;
  ref->xdg_width = width;
  ref->xdg_height = height;
  update(ref);
}

static void xdg_done(void *data, struct zxdg_output_v1 *object) {
  (void)object;
  update(data);
}

static void xdg_name(void *data, struct zxdg_output_v1 *object,
                     const char *value) {
  (void)object;
  set_name(data, value);
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
    .description = xdg_description,
};

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
    if (!ref->wl_output) {
      *ref = (output_ref_t){.id = id, .scale = 1};
      ref->wl_output = wl_registry_bind(registry, id, &wl_output_interface,
                                        version < 4 ? version : 4);
      wl_output_add_listener(ref->wl_output, &LISTENER, ref);
      attach_xdg(ref);
      changed = true;
      return;
    }
  }
}

void outputs_remove(uint32_t id) {
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    output_ref_t *ref = &outputs[i];
    if (!ref->wl_output || ref->id != id) {
      continue;
    }
    struct wl_output *object = ref->wl_output;
    if (ref->xdg_output) {
      zxdg_output_v1_destroy(ref->xdg_output);
    }
    if (wl_output_get_version(object) >= WL_OUTPUT_RELEASE_SINCE_VERSION) {
      wl_output_release(object);
    } else {
      wl_output_destroy(object);
    }
    *ref = (output_ref_t){0};
    changed = true;
    return;
  }
}

void outputs_cleanup(void) {
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    if (outputs[i].wl_output) {
      outputs_remove(outputs[i].id);
    }
  }
  output_manager = NULL;
  changed = false;
}

bool outputs_take_changed(void) {
  bool result = changed;
  changed = false;
  return result;
}
