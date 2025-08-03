#ifndef WAYLAND_H
#define WAYLAND_H

#include "core/bongocat.h"
#include "config/config.h"
#include "utils/error.h"
#include "graphics/context.h"
#include <signal.h>
#include "../protocols/zwlr-layer-shell-v1-client-protocol.h"
#include "../protocols/xdg-shell-client-protocol.h"

// Wayland globals
typedef struct {
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
    bool configured;
    bool fullscreen_detected;

    config_t *_current_config;
    animation_context_t *_anim;
} wayland_context_t;

bongocat_error_t wayland_init(wayland_context_t* ctx, config_t *config, animation_context_t* anim);
bongocat_error_t wayland_run(wayland_context_t* ctx, volatile sig_atomic_t *running);
void wayland_cleanup(wayland_context_t *ctx);
void wayland_update_config(wayland_context_t *ctx, config_t *config, animation_context_t* anim);
void draw_bar(wayland_context_t *ctx);
int create_shm(int size);
int wayland_get_screen_width(void);
const char* wayland_get_current_layer_name(void);

#endif // WAYLAND_H