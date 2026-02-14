#ifndef MULTI_MONITOR_H
#define MULTI_MONITOR_H

#include <stddef.h>

// =============================================================================
// MULTI-MONITOR SUPPORT
// =============================================================================

#define MULTI_MONITOR_MAX_OUTPUTS 16

/**
 * Launch bongocat on configured monitors via forking.
 *
 * Forks one child per configured output. Each child receives
 * `--monitor <name>` and runs the normal startup path. The parent waits
 * on all children.
 *
 * @param argc Original argc from main
 * @param argv Original argv from main
 * @param config_path Resolved config file path (may be NULL)
 * @param watch_config Whether to enable config watching
 * @param output_names List of monitor/output names
 * @param output_count Number of outputs in output_names
 * @return Exit code (0 on success)
 */
int multi_monitor_launch(int argc, char *argv[], const char *config_path,
                         int watch_config, char **output_names,
                         size_t output_count);

#endif  // MULTI_MONITOR_H
