#ifndef INPUT_H
#define INPUT_H

#include "core/bongocat.h"
#include "utils/error.h"

#include <stdatomic.h>

// =============================================================================
// INPUT STATE
// =============================================================================

// Shared pending-paw bits: input child producer, animation thread consumer.
extern atomic_uint *pending_paws;

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

// Get eventfd for waking animation thread on input events (-1 if unavailable)
int input_get_wake_fd(void);

#endif  // INPUT_H
