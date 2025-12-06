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

// Start input monitoring - must be checked
BONGOCAT_NODISCARD bongocat_error_t input_start_monitoring(char **device_paths,
                                                           int num_devices,
                                                           int enable_debug);

// Restart input monitoring with new devices - must be checked
BONGOCAT_NODISCARD bongocat_error_t input_restart_monitoring(
    char **device_paths, int num_devices, int enable_debug);

// Cleanup input monitoring resources
void input_cleanup(void);

#endif  // INPUT_H