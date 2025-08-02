#ifndef INPUT_H
#define INPUT_H

#include "bongocat.h"
#include "error.h"

bongocat_error_t input_start_monitoring(input_context_t* ctx, char **device_paths, int num_devices, int enable_debug);
bongocat_error_t input_restart_monitoring(input_context_t* ctx, char **device_paths, int num_devices, int enable_debug);
void input_cleanup(input_context_t* ctx);

#endif // INPUT_H