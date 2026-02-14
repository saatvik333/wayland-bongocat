#ifndef WAYLAND_H
#define WAYLAND_H

#include "../protocols/xdg-shell-client-protocol.h"
#include "../protocols/zwlr-layer-shell-v1-client-protocol.h"
#include "config/config.h"
#include "core/bongocat.h"
#include "utils/error.h"

#include <signal.h>
#include <stdatomic.h>

// =============================================================================
// WAYLAND GLOBAL STATE
// =============================================================================

// Core Wayland objects
extern struct wl_display *display;
extern struct wl_compositor *compositor;
extern struct wl_shm *shm;
extern struct wl_output *output;

// Layer shell objects
extern struct zwlr_layer_shell_v1 *layer_shell;
extern struct zwlr_layer_surface_v1 *layer_surface;
extern struct xdg_wm_base *xdg_wm_base;

// Surface and buffer
extern struct wl_surface *surface;
extern struct wl_buffer *buffer;
extern uint8_t *pixels;

// Thread-safe state flags
extern atomic_bool configured;
extern atomic_bool fullscreen_detected;

// =============================================================================
// WAYLAND LIFECYCLE FUNCTIONS
// =============================================================================

// Initialize Wayland connection - must be checked
BONGOCAT_NODISCARD bongocat_error_t wayland_init(config_t *config);

// Run Wayland event loop - must be checked
BONGOCAT_NODISCARD bongocat_error_t wayland_run(volatile sig_atomic_t *running);

// Cleanup Wayland resources
void wayland_cleanup(void);

// =============================================================================
// WAYLAND UTILITY FUNCTIONS
// =============================================================================

// Update configuration (hot-reload support)
void wayland_update_config(config_t *config);

// Draw the overlay bar
void draw_bar(void);

// Create shared memory buffer - returns fd or -1 on error
BONGOCAT_NODISCARD int create_shm(int size);

// Get detected screen width
BONGOCAT_NODISCARD int wayland_get_screen_width(void);

// Get detected output name
BONGOCAT_NODISCARD const char *wayland_get_output_name(void);

// Register a per-loop callback executed on Wayland main thread.
void wayland_set_tick_callback(void (*callback)(void));

// Get current layer name for logging
BONGOCAT_NODISCARD const char *wayland_get_current_layer_name(void);

#endif  // WAYLAND_H
