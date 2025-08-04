#ifndef BONGOCAT_INPUT_H
#define BONGOCAT_INPUT_H

#include "core/bongocat.h"
#include "platform/input_context.h"
#include "config/config.h"
#include "utils/error.h"

bongocat_error_t input_start_monitoring(input_context_t *ctx, const config_t* config, char **device_paths, int num_devices, int enable_debug);
bongocat_error_t input_restart_monitoring(input_context_t *ctx, const config_t* config, char **device_paths, int num_devices, int enable_debug);
void input_cleanup(input_context_t *ctx);

#endif // INPUT_H