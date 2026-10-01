#ifndef INPUT_H
#define INPUT_H

#include "core/bongocat.h"
#include "utils/error.h"

#include <stdatomic.h>

// =============================================================================
// INPUT STATE
// =============================================================================

// Renderer-local paw bits received from the private helper socket.
extern atomic_uint *pending_paws;

// =============================================================================
// INPUT MONITORING FUNCTIONS
// =============================================================================

// Start input monitoring with hotplug support - must be checked
BONGOCAT_NODISCARD bongocat_error_t
input_start_monitoring(char **paths, int num_paths, char **names, int num_names,
                       int interval, int debug);

// Restart input monitoring with new devices - must be checked
BONGOCAT_NODISCARD bongocat_error_t
input_restart_monitoring(char **paths, int num_paths, char **names,
                         int num_names, int interval, int debug);

int input_helper_main(int argc, char **argv);
int input_list_devices(void);
bool input_device_is_keyboard(int fd);
bool input_device_selected(int fd, dev_t identity, char **paths, int num_paths,
                           char **names, int num_names);
void input_process_events(void);
uint32_t input_device_count(void);
int64_t input_timestamp(void);

// Cleanup input monitoring resources
void input_cleanup(void);

// Privilege handling for setgid installs, where the binary is setgid to a
// group that may read the keyboard devices (instead of adding the user to the
// `input` group). Both are no-ops when the binary is not setgid.
//
// input_privilege_init() must be the first call in main(): it stops using the
// group (effective gid -> real gid) but keeps it in the saved set, so config
// parsing, Wayland setup and rendering run unprivileged. Rendering calls
// input_privilege_drop() before parsing; the executed helper raises its
// installed group only during device discovery/opening.
//
// input_privilege_drop() gives the group up irrevocably (real, effective and
// saved gid), and exits if that fails rather than run on with it.
void input_privilege_init(void);
void input_privilege_drop(void);

// Get child PID (async-signal-safe accessor for crash handler)
pid_t input_get_child_pid(void);

// Reap child if it exited; true while monitoring process is alive.
bool input_child_is_alive(void);

// Get the helper socket for event-loop polling (-1 if unavailable)
int input_get_wake_fd(void);

#endif  // INPUT_H
