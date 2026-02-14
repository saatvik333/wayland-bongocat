#ifndef INPUT_H
#define INPUT_H

#include "core/bongocat.h"
#include "utils/error.h"

#include <stdatomic.h>

// =============================================================================
// INPUT STATE
// =============================================================================

// Shared memory for key press state (thread-safe)
extern atomic_int *any_key_pressed;

// Last pressed key code for hand mapping (0 = none)
extern atomic_int *last_key_code;

// =============================================================================
// INPUT MONITORING FUNCTIONS
// =============================================================================

// Start input monitoring with hotplug support - must be checked
BONGOCAT_NODISCARD bongocat_error_t
input_start_monitoring(char **device_paths, int num_devices, char **names,
                       int num_names, int scan_interval, int enable_debug);

// Restart input monitoring with new devices - must be checked
BONGOCAT_NODISCARD bongocat_error_t
input_restart_monitoring(char **device_paths, int num_devices, char **names,
                         int num_names, int scan_interval, int enable_debug);

// Cleanup input monitoring resources
void input_cleanup(void);

// Get child PID (async-signal-safe accessor for crash handler)
pid_t input_get_child_pid(void);

#endif  // INPUT_H