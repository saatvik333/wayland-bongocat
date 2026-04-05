#ifndef HYPRLAND_H
#define HYPRLAND_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

// =============================================================================
// HYPRLAND-SPECIFIC TYPES
// =============================================================================

typedef struct {
  int monitor_id;  // monitor number in Hyprland
  int x, y;
  int width, height;
  bool fullscreen;
} window_info_t;

// =============================================================================
// HYPRLAND HELPER FUNCTIONS
// =============================================================================

/// Execute a command and capture its stdout into buf. Returns bytes read or -1.
/// Uses fork/execvp instead of popen to avoid shell interpretation.
ssize_t safe_exec_read(const char *const argv[], char *buf, size_t buf_size);

/// Map xdg-output names to Hyprland monitor IDs via `hyprctl monitors`.
void hypr_update_outputs_with_monitor_ids(void);

/// Query the active Hyprland window and fill @p win. Returns true on success.
bool hypr_get_active_window(window_info_t *win);

#endif  // HYPRLAND_H
