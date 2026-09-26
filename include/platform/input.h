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

// Privilege handling for setgid installs, where the binary is setgid to a
// group that may read the keyboard devices (instead of adding the user to the
// `input` group). Both are no-ops when the binary is not setgid.
//
// input_privilege_init() must be the first call in main(): it stops using the
// group (effective gid -> real gid) but keeps it in the saved set, so config
// parsing, Wayland setup and rendering run unprivileged. The input child
// raises it again only while opening devices.
//
// input_privilege_drop() gives the group up irrevocably (real, effective and
// saved gid), and exits if that fails rather than run on with it.
void input_privilege_init(void);
void input_privilege_drop(void);

// Get child PID (async-signal-safe accessor for crash handler)
pid_t input_get_child_pid(void);

// Reap child if it exited; true while monitoring process is alive.
bool input_child_is_alive(void);

// Get eventfd for waking animation thread on input events (-1 if unavailable)
int input_get_wake_fd(void);

#endif  // INPUT_H
