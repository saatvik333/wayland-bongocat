#ifndef WAYLAND_H
#define WAYLAND_H

#include "bongocat.h"
#include "config.h"
#include "error.h"
#include <signal.h>
#include "../protocols/zwlr-layer-shell-v1-client-protocol.h"

// Wayland globals
typedef struct {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wl_output *output;
    struct wl_surface *surface;
    struct wl_buffer *buffer;
    struct zwlr_layer_surface_v1 *layer_surface;
    uint8_t *pixels;
    bool configured;

    config_t *current_config;

    // Screen dimensions from Wayland output
    int _wayland_screen_width;
    int _wayland_screen_height;
    int _wayland_transform;
    int _wayland_raw_width;
    int _wayland_raw_height;
    bool _wayland_mode_received;
    bool _wayland_geometry_received;
} wayland_context_t;

bongocat_error_t wayland_init(wayland_context_t* ctx, animation_context_t* anim, config_t *config);
bongocat_error_t wayland_run(wayland_context_t* ctx, volatile sig_atomic_t *running);
void wayland_cleanup(wayland_context_t* ctx);
void wayland_update_config(wayland_context_t* ctx, config_t *config, animation_context_t* anim);
void draw_bar(wayland_context_t* ctx, animation_context_t* anim);
int create_shm(int size);
int wayland_get_screen_width(wayland_context_t* ctx);

#endif // WAYLAND_H