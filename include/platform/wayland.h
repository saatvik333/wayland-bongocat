#ifndef WAYLAND_H
#define WAYLAND_H

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
// Surface
extern struct wl_surface *surface;

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
// WAYLAND OUTPUT STATE (shared with fullscreen / hyprland modules)
// =============================================================================

// Output reference array and count (defined in wayland.c)
extern output_ref_t outputs[];
extern size_t output_count;

// =============================================================================
// WAYLAND UTILITY FUNCTIONS
// =============================================================================

// Update configuration (hot-reload support)
void wayland_update_config(config_t *config);

// Draw the overlay bar
void draw_bar(void);
void wayland_request_redraw(void);

// Get the wl_output associated with the current screen info (may be NULL)
BONGOCAT_NODISCARD struct wl_output *wayland_get_current_screen_output(void);

// Register a per-loop callback executed on Wayland main thread.
void wayland_set_tick_callback(void (*callback)(void));

// HiDPI: convert a logical-pixel dimension to physical (buffer-coordinate)
// pixels using the active render scale. Defaults to identity (scale 1.0×) if
// the compositor has not announced a scale yet.
BONGOCAT_NODISCARD int wayland_phys_dim(int logical);

#endif  // WAYLAND_H
