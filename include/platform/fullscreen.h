#ifndef FULLSCREEN_H
#define FULLSCREEN_H

#include <stdbool.h>
#include <wayland-client.h>

// Forward-declare the protocol type so callers don't need to include
// the generated header just for the listener pointer.
struct zwlr_foreign_toplevel_manager_v1;
struct zwlr_foreign_toplevel_manager_v1_listener;

// =============================================================================
// FULLSCREEN DETECTION PUBLIC API
// =============================================================================

/// Set up the foreign-toplevel manager listener after binding the manager
/// during registry_global.  Call this once the manager proxy is valid.
void fullscreen_init(struct zwlr_foreign_toplevel_manager_v1 *manager);

/// Clean up tracked toplevels and manager.  Safe to call even if
/// fullscreen_init was never called.
void fullscreen_cleanup(void);

/// Returns the current fullscreen detection state (true = a fullscreen
/// toplevel is covering our output).
bool fullscreen_is_detected(void);

/// Return the listener struct that must be attached when binding the
/// foreign-toplevel-manager in registry_global.
const struct zwlr_foreign_toplevel_manager_v1_listener *
fullscreen_get_manager_listener(void);

#endif  // FULLSCREEN_H
