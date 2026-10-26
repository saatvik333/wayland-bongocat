#ifndef NIRI_H
#define NIRI_H

#include <stdbool.h>

/// Subscribe to, or drop, the niri event stream named by NIRI_SOCKET. Without
/// niri no socket is opened; a failed connection is retried only after the
/// tracker is disabled and enabled again.
void niri_set_enabled(bool value);

/// Drain pending events without blocking. Closes the stream on EOF or errors.
void niri_poll(void);
/// Stream to poll; -1 while disconnected.
int niri_fd(void);

/// Output of niri's focused workspace, including empty workspaces. NULL while
/// unknown, such as before the initial workspace list arrives.
const char *niri_focused_output(void);
/// Whether the focused workspace holds no window.
bool niri_focused_empty(void);
/// True once after the focused output or its emptiness changed.
bool niri_take_changed(void);

void niri_cleanup(void);

#endif  // NIRI_H
