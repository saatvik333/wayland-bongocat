#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "platform/input.h"
#include "graphics/animation.h"
#include "utils/memory.h"
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <linux/input.h>
#include <sys/inotify.h>
#include <signal.h>
#include <unistd.h>
#include <assert.h>
#include <fcntl.h>

// Child process signal handler - exits quietly without logging
static void child_signal_handler(int sig) {
    (void)sig; // Suppress unused parameter warning
    exit(0);
}

static void capture_input_multiple(input_context_t *input, char **device_paths, int num_devices, int enable_debug) {
    assert(input);

    // Set up child-specific signal handlers to avoid duplicate logging
    struct sigaction sa;
    sa.sa_handler = child_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    bongocat_log_debug("Starting input capture on %d devices", num_devices);

    int *fds = BONGOCAT_MALLOC(num_devices * sizeof(int));
    char **unique_paths = BONGOCAT_MALLOC(num_devices * sizeof(char*));
    if (!fds || !unique_paths) {
        BONGOCAT_SAFE_FREE(fds);
        BONGOCAT_SAFE_FREE(unique_paths);
        atomic_store(&input->_capture_input_running, false);
        bongocat_log_error("Failed to allocate memory for file descriptors");
        exit(1);
    }
    
    int max_fd = -1;
    int valid_devices = 0;
    int unique_devices = 0;
    
    // First pass: deduplicate device paths
    for (int i = 0; i < num_devices; i++) {
        bool is_duplicate = false;
        for (int j = 0; j < unique_devices; j++) {
            if (strcmp(device_paths[i], unique_paths[j]) == 0) {
                is_duplicate = true;
                break;
            }
        }
        if (!is_duplicate) {
            unique_paths[unique_devices] = device_paths[i];
            unique_devices++;
        }
    }
    
    bongocat_log_debug("Deduplicated %d devices to %d unique devices", num_devices, unique_devices);
    
    // Open all unique devices
    for (int i = 0; i < unique_devices; i++) {
        fds[i] = -1;
        
        // Validate device path exists and is readable
        struct stat st;
        if (stat(unique_paths[i], &st) != 0) {
            bongocat_log_warning("Input device does not exist: %s", unique_paths[i]);
            continue;
        }
        
        if (!S_ISCHR(st.st_mode)) {
            bongocat_log_warning("Input device is not a character device: %s", unique_paths[i]);
            continue;
        }
        
        fds[i] = open(unique_paths[i], O_RDONLY | O_NONBLOCK);
        if (fds[i] < 0) {
            bongocat_log_warning("Failed to open %s: %s", unique_paths[i], strerror(errno));
            continue;
        }
        
        bongocat_log_info("Input monitoring started on %s (fd=%d)", unique_paths[i], fds[i]);
        if (fds[i] > max_fd) {
            max_fd = fds[i];
        }
        valid_devices++;
    }
    
    // Update num_devices to reflect unique devices for the rest of the function
    num_devices = unique_devices;
    
    if (valid_devices == 0) {
        atomic_store(&input->_capture_input_running, false);
        bongocat_log_error("No valid input devices found");
        BONGOCAT_SAFE_FREE(fds);
        exit(1);
    }
    
    bongocat_log_info("Successfully opened %d/%d input devices", valid_devices, num_devices);

    struct input_event ev[128]; // Increased buffer size for better I/O efficiency
    int rd;
    fd_set readfds;
    struct timeval timeout;
    int check_counter = 0;
    int adaptive_check_interval = 5; // Start with 5 seconds, can increase to 30

    atomic_store(&input->_capture_input_running, true);
    while (atomic_load(&input->_capture_input_running)) {
        FD_ZERO(&readfds);
        
        // Optimize: only rebuild fd_set when devices change, track current max_fd
        int current_max_fd = -1;
        for (int i = 0; i < num_devices; i++) {
            if (fds[i] >= 0) {
                FD_SET(fds[i], &readfds);
                if (fds[i] > current_max_fd) {
                    current_max_fd = fds[i];
                }
            }
        }
        max_fd = current_max_fd;
        
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        int select_result = select(max_fd + 1, &readfds, NULL, NULL, &timeout);
        if (select_result < 0) {
            if (errno == EINTR) continue; // Interrupted by signal
            bongocat_log_error("Select error: %s", strerror(errno));
            break;
        }
        
        if (select_result == 0) {
            // Adaptive device checking - start at 5 seconds, increase to 30 if no new devices found
            check_counter++;
            if (check_counter >= adaptive_check_interval) {
                check_counter = 0;
                bool found_new_device = false;
                
                // Check for devices that have become available
                for (int i = 0; i < num_devices; i++) {
                    if (fds[i] < 0) { // Device was not available before
                        struct stat st;
                        if (stat(unique_paths[i], &st) == 0 && S_ISCHR(st.st_mode)) {
                            // Device is now available, try to open it
                            int new_fd = open(unique_paths[i], O_RDONLY | O_NONBLOCK);
                            if (new_fd >= 0) {
                                fds[i] = new_fd;
                                if (new_fd > max_fd) {
                                    max_fd = new_fd;
                                }
                                valid_devices++;
                                found_new_device = true;
                                bongocat_log_info("New input device detected and opened: %s (fd=%d)", unique_paths[i], new_fd);
                            }
                        }
                    }
                }
                
                // Adaptive interval: if no new devices found, increase check interval up to 30 seconds
                if (!found_new_device && adaptive_check_interval < 30) {
                    adaptive_check_interval = (adaptive_check_interval < 15) ? 15 : 30;
                    bongocat_log_debug("Increased device check interval to %d seconds", adaptive_check_interval);
                } else if (found_new_device && adaptive_check_interval > 5) {
                    // Reset to frequent checking when devices are being connected
                    adaptive_check_interval = 5;
                    bongocat_log_debug("Reset device check interval to 5 seconds");
                }
            }
            continue; // Continue to next iteration
        }
        
        // Check which devices have data
        for (int i = 0; i < num_devices; i++) {
            if (fds[i] >= 0 && FD_ISSET(fds[i], &readfds)) {
                rd = read(fds[i], ev, sizeof(ev));
                if (rd < 0) {
                    if (errno == EAGAIN) continue;
                    bongocat_log_warning("Read error on %s: %s", unique_paths[i], strerror(errno));
                    close(fds[i]);
                    fds[i] = -1;
                    valid_devices--;
                    continue;
                }
                
                if (rd == 0) {
                    bongocat_log_warning("EOF on input device %s", unique_paths[i]);
                    close(fds[i]);
                    fds[i] = -1;
                    valid_devices--;
                    continue;
                }

                // Batch process events for better performance
                int num_events = rd / sizeof(struct input_event);
                bool key_pressed = false;
                
                for (int j = 0; j < num_events; j++) {
                    if (ev[j].type == EV_KEY && ev[j].value == 1) {
                        key_pressed = true;
                        if (enable_debug) {
                            bongocat_log_debug("Key event: device=%s, code=%d, time=%ld.%06ld", 
                                             unique_paths[i], ev[j].code, ev[j].time.tv_sec, ev[j].time.tv_usec);
                        }
                    }
                }
                
                // Trigger animation only once per batch to reduce overhead
                if (key_pressed) {
                    // update KPM
                    const timestamp_ms_t now = get_current_time_ms();
                    if (input->_latest_pressed_key_timestamp_ms != now) {
                        const time_ms_t duration_ms = now - input->_latest_pressed_key_timestamp_ms;

                        const double duration_min = duration_ms / 60000.0;
                        input->kpm = input->_input_kpm_counter / duration_min;

                        input->_input_kpm_counter = 0;
                        bongocat_log_debug("Input KPM: %d, delta: %dms", input->kpm, duration_ms);
                    }
                    input->_latest_pressed_key_timestamp_ms = now;

                    input->input_counter++;
                    input->_input_kpm_counter++;
                    animation_trigger(input);
                }
            }
        }
        
        // Exit if no valid devices remain
        if (valid_devices == 0) {
            bongocat_log_error("All input devices became unavailable");
            break;
        }
    }
    atomic_store(&input->_capture_input_running, false);

    // Close all file descriptors
    for (int i = 0; i < num_devices; i++) {
        if (fds[i] >= 0) {
            close(fds[i]);
        }
    }
    
    BONGOCAT_SAFE_FREE(fds);
    BONGOCAT_SAFE_FREE(unique_paths);
    bongocat_log_info("Input monitoring stopped");
}

bongocat_error_t input_start_monitoring(input_context_t* ctx, char **device_paths, int num_devices, int enable_debug) {
    BONGOCAT_CHECK_NULL(device_paths, BONGOCAT_ERROR_INVALID_PARAM);
    
    if (num_devices <= 0) {
        bongocat_log_error("No input devices specified");
        return BONGOCAT_ERROR_INVALID_PARAM;
    }

    ctx->_input_child_pid = -1;
    ctx->_capture_input_running = false;
    ctx->input_counter = 0;
    ctx->_input_kpm_counter = 0;
    ctx->_latest_pressed_key_timestamp_ms = get_current_time_ms();

    bongocat_log_info("Initializing input monitoring system for %d devices", num_devices);
    
    // Initialize shared memory for key press flag
    ctx->any_key_pressed = (int *)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ctx->any_key_pressed == MAP_FAILED) {
        bongocat_log_error("Failed to create shared memory for input monitoring: %s", strerror(errno));
        return BONGOCAT_ERROR_MEMORY;
    }
    *ctx->any_key_pressed = 0;

    // Fork process for input monitoring
    ctx->_input_child_pid = fork();
    if (ctx->_input_child_pid < 0) {
        atomic_store(&ctx->_capture_input_running, false);
        bongocat_log_error("Failed to fork input monitoring process: %s", strerror(errno));
        munmap(ctx->any_key_pressed, sizeof(int));
        ctx->any_key_pressed = NULL;
        return BONGOCAT_ERROR_THREAD;
    }
    if (ctx->_input_child_pid == 0) {
        // Child process - handle keyboard input from multiple devices
        bongocat_log_debug("Input monitoring child process started (PID: %d)", getpid());
        capture_input_multiple(ctx, device_paths, num_devices, enable_debug);
        exit(0);
    }
    
    bongocat_log_info("Input monitoring started (child PID: %d)", ctx->_input_child_pid);
    return BONGOCAT_SUCCESS;
}

bongocat_error_t input_restart_monitoring(input_context_t* ctx, char **device_paths, int num_devices, int enable_debug) {
    bongocat_log_info("Restarting input monitoring system");

    ctx->_input_kpm_counter = 0;
    ctx->_latest_pressed_key_timestamp_ms = get_current_time_ms();
    
    // Stop current monitoring
    if (ctx->_input_child_pid > 0) {
        bongocat_log_debug("Stopping current input monitoring (PID: %d)", ctx->_input_child_pid);
        kill(ctx->_input_child_pid, SIGTERM);
        
        // Wait for child to terminate with timeout
        int status;
        int wait_attempts = 0;
        while (wait_attempts < 10) {
            pid_t result = waitpid(ctx->_input_child_pid, &status, WNOHANG);
            if (result == ctx->_input_child_pid) {
                atomic_store(&ctx->_capture_input_running, false);
                bongocat_log_debug("Previous input monitoring process terminated");
                break;
            } else if (result == -1 && ctx->_capture_input_running) {
                atomic_store(&ctx->_capture_input_running, false);
                bongocat_log_warning("Error waiting for input child process: %s", strerror(errno));
                break;
            }
            
            usleep(100000); // Wait 100ms
            wait_attempts++;
        }
        
        // Force kill if still running
        if (wait_attempts >= 10) {
            atomic_store(&ctx->_capture_input_running, false);
            bongocat_log_warning("Force killing previous input monitoring process");
            kill(ctx->_input_child_pid, SIGKILL);
            waitpid(ctx->_input_child_pid, &status, 0);
        }
        
        ctx->_input_child_pid = -1;
    }
    
    // Start new monitoring (reuse shared memory if it exists)
    bool need_new_shm = (ctx->any_key_pressed == NULL || ctx->any_key_pressed == MAP_FAILED);
    
    if (need_new_shm) {
        ctx->any_key_pressed = (int *)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (ctx->any_key_pressed == MAP_FAILED) {
            bongocat_log_error("Failed to create shared memory for input monitoring: %s", strerror(errno));
            return BONGOCAT_ERROR_MEMORY;
        }
        *ctx->any_key_pressed = 0;
    }

    // Fork new process for input monitoring
    ctx->_input_child_pid = fork();
    if (ctx->_input_child_pid < 0) {
        bongocat_log_error("Failed to fork input monitoring process: %s", strerror(errno));
        if (need_new_shm) {
            munmap(ctx->any_key_pressed, sizeof(int));
            ctx->any_key_pressed = NULL;
        }
        return BONGOCAT_ERROR_THREAD;
    }
    
    if (ctx->_input_child_pid == 0) {
        atomic_store(&ctx->_capture_input_running, false);
        // Child process - handle keyboard input from multiple devices
        bongocat_log_debug("Input monitoring child process restarted (PID: %d)", getpid());
        capture_input_multiple(ctx, device_paths, num_devices, enable_debug);
        exit(0);
    }
    
    bongocat_log_info("Input monitoring restarted (child PID: %d)", ctx->_input_child_pid);
    return BONGOCAT_SUCCESS;
}

void input_cleanup(input_context_t* ctx) {
    bongocat_log_info("Cleaning up input monitoring system");
    
    // Terminate child process if it exists
    if (ctx->_input_child_pid > 0) {
        atomic_store(&ctx->_capture_input_running, false);
        bongocat_log_debug("Terminating input monitoring child process (PID: %d)", ctx->_input_child_pid);
        kill(ctx->_input_child_pid, SIGTERM);
        
        // Wait for child to terminate with timeout
        int status;
        int wait_attempts = 0;
        while (wait_attempts < 10) {
            pid_t result = waitpid(ctx->_input_child_pid, &status, WNOHANG);
            if (result == ctx->_input_child_pid) {
                atomic_store(&ctx->_capture_input_running, false);
                bongocat_log_debug("Input monitoring child process terminated gracefully");
                break;
            } else if (result == -1 && ctx->_capture_input_running) {
                atomic_store(&ctx->_capture_input_running, false);
                bongocat_log_warning("Error waiting for input child process: %s", strerror(errno));
                break;
            }
            
            usleep(100000); // Wait 100ms
            wait_attempts++;
        }
        
        // Force kill if still running
        if (wait_attempts >= 10) {
            atomic_store(&ctx->_capture_input_running, false);
            bongocat_log_warning("Force killing input monitoring child process");
            kill(ctx->_input_child_pid, SIGKILL);
            waitpid(ctx->_input_child_pid, &status, 0);
        }
        
        ctx->_input_child_pid = -1;
    }
    
    // Cleanup shared memory
    if (ctx->any_key_pressed && ctx->any_key_pressed != MAP_FAILED) {
        munmap(ctx->any_key_pressed, sizeof(int));
        ctx->any_key_pressed = NULL;
    }

    ctx->kpm = 0;
    ctx->input_counter = 0;
    ctx->_input_kpm_counter = 0;
    ctx->_latest_pressed_key_timestamp_ms = 0;
    
    bongocat_log_debug("Input monitoring cleanup complete");
}