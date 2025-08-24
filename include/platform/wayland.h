#ifndef WAYLAND_H
#define WAYLAND_H

#include "core/bongocat.h"
#include "config/config.h"
#include "utils/error.h"
#include <signal.h>
#include "../protocols/zwlr-layer-shell-v1-client-protocol.h"
#include "../protocols/xdg-shell-client-protocol.h"

// Wayland globals
extern struct wl_display *display;
extern struct wl_compositor *compositor;
extern struct wl_shm *shm;
extern struct zwlr_layer_shell_v1 *layer_shell;
extern struct xdg_wm_base *xdg_wm_base;
extern struct wl_output *output;
extern struct wl_surface *surface;
extern struct wl_buffer *buffer;
extern struct zwlr_layer_surface_v1 *layer_surface;
extern uint8_t *pixels;
extern bool configured;
extern bool fullscreen_detected;

typedef void (*config_reload_callback_t)();

bongocat_error_t wayland_init(config_t *config);
bongocat_error_t wayland_run(volatile sig_atomic_t *running, int signal_fd, config_reload_callback_t config_reload_callback);
void wayland_cleanup(void);
void wayland_update_config(config_t *config);
void draw_bar(void);
int create_shm(int size);
int wayland_get_screen_width(void);
const char* wayland_get_current_layer_name(void);

#endif // WAYLAND_H