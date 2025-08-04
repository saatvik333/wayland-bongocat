#ifndef BONGOCAT_INPUT_CONTEXT_H
#define BONGOCAT_INPUT_CONTEXT_H

#include "utils/time.h"
#include <pthread.h>
#include <stdatomic.h>

typedef struct {
    /// @NOTE: variables can be shared between child process and parent
    int *any_key_pressed;
    int *kpm;                    // keystrokes per minute
    atomic_int *input_counter;

    pid_t _input_child_pid;
    atomic_bool _capture_input_running;
    atomic_int _input_kpm_counter;
    timestamp_ms_t _latest_kpm_update_ms;
} input_context_t;

#endif // BONGOCAT_INPUT_CONTEXT_H