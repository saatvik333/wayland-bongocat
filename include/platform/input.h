#ifndef INPUT_H
#define INPUT_H

#include "core/bongocat.h"
#include "utils/error.h"

extern int *any_key_pressed;

bongocat_error_t input_start_monitoring(char **device_paths, int num_devices,
                                        int enable_debug);
bongocat_error_t input_restart_monitoring(char **device_paths, int num_devices,
                                          int enable_debug);
void input_cleanup(void);

#endif // INPUT_H