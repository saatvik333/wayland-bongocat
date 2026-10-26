#ifndef INPUT_H
#define INPUT_H

#include "config/config.h"
#include "utils/error.h"

#include <stdbool.h>
#include <stdint.h>

// =============================================================================
// RENDERER SIDE
// =============================================================================
//
// Keyboards are read by a helper process (`bongocat --input-helper`) that
// reports only paw bits, timestamps and device counts over a socket. The
// socket reaches end-of-file when the helper exits.

[[nodiscard]] bongocat_error_t input_start(const config_t *config);
// Kills and reaps the helper.
void input_stop(void);
bool input_running(void);
// Socket to poll for helper messages; -1 while no helper runs.
int input_fd(void);
// Drains helper messages and returns the PAW_* bits pressed since the last
// call. *time receives the newest press time (monotonic microseconds) when
// any paw was pressed. Reaps the helper when it has exited.
unsigned input_read(int64_t *time);
uint32_t input_device_count(void);

// Prints every evdev device and suggested settings for keyboards.
int input_list_devices(void);

// Entry point of the helper process.
int input_helper_main(int argc, char **argv);

// =============================================================================
// SETGID PRIVILEGES
// =============================================================================
//
// For installs where the binary is setgid to a group that may read keyboards.
// No-ops otherwise. input_privilege_init() must be the first call in main():
// it stops using the group but keeps it in the saved set. The renderer calls
// input_privilege_drop() before touching configuration, which gives the group
// up irrevocably (or exits). Only the helper and device listing raise it,
// and only while opening devices.
void input_privilege_init(void);
void input_privilege_drop(void);

#endif  // INPUT_H
