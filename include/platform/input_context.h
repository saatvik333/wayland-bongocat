#ifndef BONGOCAT_INPUT_CONTEXT_H
#define BONGOCAT_INPUT_CONTEXT_H

#include "utils/time.h"
#include <pthread.h>
#include <stdatomic.h>

typedef struct {
    int *any_key_pressed;
    // keystrokes per minute
    int kpm;
    atomic_int input_counter;

    pid_t _input_child_pid;
    atomic_bool _capture_input_running;
    atomic_int _input_kpm_counter;
    timestamp_ms_t _latest_pressed_key_timestamp_ms;
} input_context_t;

#endif // BONGOCAT_INPUT_CONTEXT_H