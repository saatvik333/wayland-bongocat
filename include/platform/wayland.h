#ifndef WAYLAND_H
#define WAYLAND_H

#include "config/config.h"
#include "utils/error.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/poll.h>

// Connects and binds globals. settings must stay valid until the next
// wayland_update_config() or wayland_cleanup().
[[nodiscard]] bongocat_error_t wayland_init(const config_t *settings);
void wayland_update_config(const config_t *settings);
void wayland_cleanup(void);

void wayland_set_hidden(bool value);
// Paused overlays show the idle frame; pausing and resuming reset timers.
void wayland_set_paused(bool value);
// Scheduled sleep shows the sleeping frame.
void wayland_set_sleeping(bool value);
// Queues PAW_* bits pressed at monotonic time (microseconds; 0 if unknown).
void wayland_add_paws(unsigned paws, int64_t time);

// One event-loop turn: wayland_prepare() dispatches queued events, updates and
// renders overlays, flushes, and fills pfd. *deadline receives the next
// monotonic microsecond time overlays need attention, or 0. After poll(),
// wayland_dispatch() must be called with the same pfd.
[[nodiscard]] bongocat_error_t wayland_prepare(struct pollfd *pfd,
                                               int64_t *deadline);
[[nodiscard]] bongocat_error_t wayland_dispatch(const struct pollfd *pfd);

// Prints outputs (and protocol support with doctor). Returns nonzero when
// overlays cannot be shown.
int wayland_list_monitors(bool doctor);

#endif  // WAYLAND_H
