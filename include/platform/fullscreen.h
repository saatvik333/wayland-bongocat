#ifndef FULLSCREEN_H
#define FULLSCREEN_H

#include <stdbool.h>

struct wl_output;
struct zwlr_foreign_toplevel_manager_v1;

// Tracks toplevel focus and fullscreen state with
// wlr-foreign-toplevel-management. Each output remembers the window focused
// on it most recently, so a fullscreen window keeps its output covered while
// focus is on another output.

void fullscreen_init(struct zwlr_foreign_toplevel_manager_v1 *object);
void fullscreen_cleanup(void);
bool fullscreen_available(void);

// Whether the window most recently focused on output is fullscreen.
bool fullscreen_on(struct wl_output *output);
// Output of the focused window, preferring current for a window spanning
// outputs. NULL while no window is focused or its output is unknown.
struct wl_output *fullscreen_focused_output(struct wl_output *current);
// Forgets the windows focused on output so far, after an empty workspace
// gained focus there.
void fullscreen_forget(struct wl_output *output);
void fullscreen_output_removed(struct wl_output *output);
// Applies focus changes once a batch of events has been dispatched.
void fullscreen_settle(void);
// True once after focus, placement or fullscreen state changed.
bool fullscreen_take_changed(void);

#endif  // FULLSCREEN_H
