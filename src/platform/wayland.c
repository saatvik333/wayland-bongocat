#define _POSIX_C_SOURCE 200809L
#include "platform/wayland.h"
#include "graphics/animation.h"
#include "../protocols/wlr-foreign-toplevel-management-v1-client-protocol.h"
#include "../protocols/xdg-output-unstable-v1-client-protocol.h"
#include <poll.h>
#include <sys/time.h>
#include <sys/signalfd.h>
#include <sys/wait.h>

// =============================================================================
// GLOBAL STATE AND CONFIGURATION
// =============================================================================

// Wayland globals
bool configured = false;
bool fullscreen_detected = false;
struct wl_display *display;
struct wl_compositor *compositor;
struct wl_shm *shm;
struct zwlr_layer_shell_v1 *layer_shell;
struct xdg_wm_base *xdg_wm_base;
struct wl_output *output;
struct wl_surface *surface;
struct wl_buffer *buffer;
struct zwlr_layer_surface_v1 *layer_surface;
uint8_t *pixels;

static config_t *current_config;

// =============================================================================
// SCREEN DIMENSION MANAGEMENT
// =============================================================================

typedef struct {
    int screen_width;
    int screen_height;
    int transform;
    int raw_width;
    int raw_height;
    bool mode_received;
    bool geometry_received;
} screen_info_t;

static screen_info_t screen_info = {0};
static output_ref_t outputs[MAX_OUTPUTS];
static size_t output_count = 0;
static struct zxdg_output_manager_v1 *xdg_output_manager = NULL;

// =============================================================================
// ZXDG LISTENER IMPLEMENTATION
// =============================================================================

static void handle_xdg_output_name(void *data, struct zxdg_output_v1 *xdg_output __attribute__((unused)),
                                   const char *name) {
    output_ref_t *oref = data;
    snprintf(oref->name_str, sizeof(oref->name_str), "%s", name);
    oref->name_received = true;
    bongocat_log_debug("xdg-output name received: %s", name);
}

static void handle_xdg_output_logical_position(void *data, struct zxdg_output_v1 *xdg_output,
                                               int32_t x, int32_t y) {}
static void handle_xdg_output_logical_size(void *data, struct zxdg_output_v1 *xdg_output,
                                           int32_t width, int32_t height) {}
static void handle_xdg_output_done(void *data, struct zxdg_output_v1 *xdg_output) {}

static void handle_xdg_output_description(void *data, struct zxdg_output_v1 *xdg_output, const char *description) {
    (void)data; (void)xdg_output; (void)description;
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_position = handle_xdg_output_logical_position,
    .logical_size = handle_xdg_output_logical_size,
    .done = handle_xdg_output_done,
    .name = handle_xdg_output_name,
    .description = handle_xdg_output_description
};

// =============================================================================
// FULLSCREEN DETECTION MODULE
// =============================================================================

typedef struct {
    struct zwlr_foreign_toplevel_manager_v1 *manager;
    bool has_fullscreen_toplevel;
    struct timeval last_check;
} fullscreen_detector_t;

static fullscreen_detector_t fs_detector = {0};

// =============================================================================
// FULLSCREEN DETECTION IMPLEMENTATION
// =============================================================================

static void fs_update_state(bool new_state) {
    if (new_state != fs_detector.has_fullscreen_toplevel) {
        fs_detector.has_fullscreen_toplevel = new_state;
        fullscreen_detected = new_state;
        
        bongocat_log_info("Fullscreen state changed: %s", 
                         fullscreen_detected ? "detected" : "cleared");
        
        if (configured) {
            draw_bar();
        }
    }
}

static bool fs_check_compositor_fallback(void) {
    bongocat_log_debug("Using compositor-specific fullscreen detection");
    
    // Try Hyprland first
    FILE *fp = popen("hyprctl activewindow 2>/dev/null", "r");
    if (fp) {
        char line[512];
        bool is_fullscreen = false;
        
        while (fgets(line, sizeof(line), fp)) {
            size_t len = strlen(line);
            if (len > 0 && line[len-1] == '\n') {
                line[len-1] = '\0';
            }
            
            if (strstr(line, "fullscreen: 1") || strstr(line, "fullscreen: 2") || 
                strstr(line, "fullscreen: true")) {
                is_fullscreen = true;
                bongocat_log_debug("Fullscreen detected in Hyprland");
                break;
            }
        }
        pclose(fp);
        return is_fullscreen;
    }
    
    // Try Sway as fallback
    fp = popen("swaymsg -t get_tree 2>/dev/null", "r");
    if (fp) {
        char sway_buffer[4096];
        bool is_fullscreen = false;
        
        while (fgets(sway_buffer, sizeof(sway_buffer), fp)) {
            if (strstr(sway_buffer, "\"fullscreen_mode\":1")) {
                is_fullscreen = true;
                bongocat_log_debug("Fullscreen detected in Sway");
                break;
            }
        }
        pclose(fp);
        return is_fullscreen;
    }
    
    bongocat_log_debug("No supported compositor found for fullscreen detection");
    return false;
}

static bool fs_check_status(void) {
    if (fs_detector.manager) {
        return fs_detector.has_fullscreen_toplevel;
    }
    return fs_check_compositor_fallback();
}

// Foreign toplevel protocol event handlers
static void fs_handle_toplevel_state(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle, 
                                     struct wl_array *state) {
    (void)data; (void)handle;
    
    bool is_fullscreen = false;
    uint32_t *state_ptr;
    
    wl_array_for_each(state_ptr, state) {
        if (*state_ptr == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN) {
            is_fullscreen = true;
            break;
        }
    }
    
    fs_update_state(is_fullscreen);
}

static void fs_handle_toplevel_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle) {
    (void)data;
    zwlr_foreign_toplevel_handle_v1_destroy(handle);
}

// Minimal event handlers for unused events
static void fs_handle_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle, const char *title) {
    (void)data; (void)handle; (void)title;
}

static void fs_handle_app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle, const char *app_id) {
    (void)data; (void)handle; (void)app_id;
}

static void fs_handle_output_enter(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle, struct wl_output *output) {
    (void)data; (void)handle; (void)output;
}

static void fs_handle_output_leave(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle, struct wl_output *output) {
    (void)data; (void)handle; (void)output;
}

static void fs_handle_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle) {
    (void)data; (void)handle;
}

static void fs_handle_parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle, struct zwlr_foreign_toplevel_handle_v1 *parent) {
    (void)data; (void)handle; (void)parent;
}

static const struct zwlr_foreign_toplevel_handle_v1_listener fs_toplevel_listener = {
    .title = fs_handle_title,
    .app_id = fs_handle_app_id,
    .output_enter = fs_handle_output_enter,
    .output_leave = fs_handle_output_leave,
    .state = fs_handle_toplevel_state,
    .done = fs_handle_done,
    .closed = fs_handle_toplevel_closed,
    .parent = fs_handle_parent,
};

static void fs_handle_manager_toplevel(void *data, struct zwlr_foreign_toplevel_manager_v1 *manager, 
                                      struct zwlr_foreign_toplevel_handle_v1 *toplevel) {
    (void)data; (void)manager;
    
    zwlr_foreign_toplevel_handle_v1_add_listener(toplevel, &fs_toplevel_listener, NULL);
    bongocat_log_debug("New toplevel registered for fullscreen monitoring");
}

static void fs_handle_manager_finished(void *data, struct zwlr_foreign_toplevel_manager_v1 *manager) {
    (void)data;
    bongocat_log_info("Foreign toplevel manager finished");
    zwlr_foreign_toplevel_manager_v1_destroy(manager);
    fs_detector.manager = NULL;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener fs_manager_listener = {
    .toplevel = fs_handle_manager_toplevel,
    .finished = fs_handle_manager_finished,
};

// =============================================================================
// SCREEN DIMENSION MANAGEMENT
// =============================================================================

static void screen_calculate_dimensions(void) {
    if (!screen_info.mode_received || !screen_info.geometry_received || screen_info.screen_width > 0) {
        return;
    }
    
    bool is_rotated = (screen_info.transform == WL_OUTPUT_TRANSFORM_90 ||
                      screen_info.transform == WL_OUTPUT_TRANSFORM_270 ||
                      screen_info.transform == WL_OUTPUT_TRANSFORM_FLIPPED_90 ||
                      screen_info.transform == WL_OUTPUT_TRANSFORM_FLIPPED_270);
    
    if (is_rotated) {
        screen_info.screen_width = screen_info.raw_height;
        screen_info.screen_height = screen_info.raw_width;
        bongocat_log_info("Detected rotated screen: %dx%d (transform: %d)", 
                         screen_info.raw_height, screen_info.raw_width, screen_info.transform);
    } else {
        screen_info.screen_width = screen_info.raw_width;
        screen_info.screen_height = screen_info.raw_height;
        bongocat_log_info("Detected screen: %dx%d (transform: %d)", 
                         screen_info.raw_width, screen_info.raw_height, screen_info.transform);
    }
}

// =============================================================================
// BUFFER AND DRAWING MANAGEMENT
// =============================================================================

int create_shm(int size) {
    char name[] = "/bar-shm-XXXXXX";
    int fd;

    for (int i = 0; i < 100; i++) {
        for (int j = 0; j < 6; j++) {
            name[9 + j] = 'A' + (rand() % 26);
        }
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            shm_unlink(name);
            break;
        }
    }

    if (fd < 0 || ftruncate(fd, size) < 0) {
        perror("shm");
        exit(1);
    }

    return fd;
}

void draw_bar(void) {
    if (!configured) {
        bongocat_log_debug("Surface not configured yet, skipping draw");
        return;
    }

    int effective_opacity = fullscreen_detected ? 0 : current_config->overlay_opacity;
    
    // Clear buffer with transparency
    for (int i = 0; i < current_config->screen_width * current_config->bar_height * 4; i += 4) {
        pixels[i] = 0;       // B
        pixels[i + 1] = 0;   // G
        pixels[i + 2] = 0;   // R
        pixels[i + 3] = effective_opacity; // A
    }

    // Draw cat if visible
    if (!fullscreen_detected) {
        pthread_mutex_lock(&anim_lock);
        int cat_height = current_config->cat_height;
        int cat_width = (cat_height * 779) / 320;
        int cat_x = (current_config->screen_width - cat_width) / 2 + current_config->cat_x_offset;
        int cat_y = (current_config->bar_height - cat_height) / 2 + current_config->cat_y_offset;

        blit_image_scaled(pixels, current_config->screen_width, current_config->bar_height,
                          anim_imgs[anim_index], anim_width[anim_index], anim_height[anim_index],
                          cat_x, cat_y, cat_width, cat_height);
        pthread_mutex_unlock(&anim_lock);
    } else {
        bongocat_log_debug("Cat hidden due to fullscreen detection");
    }

    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, current_config->screen_width, current_config->bar_height);
    wl_surface_commit(surface);
    wl_display_flush(display);
}

// =============================================================================
// WAYLAND EVENT HANDLERS
// =============================================================================

static void layer_surface_configure(void *data __attribute__((unused)),
                                   struct zwlr_layer_surface_v1 *ls,
                                   uint32_t serial, uint32_t w, uint32_t h) {
    bongocat_log_debug("Layer surface configured: %dx%d", w, h);
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    configured = true;
    draw_bar();
}

static struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_surface_configure,
    .closed = NULL,
};

static void xdg_wm_base_ping(void *data __attribute__((unused)), 
                            struct xdg_wm_base *wm_base, uint32_t serial) {
    xdg_wm_base_pong(wm_base, serial);
}

static struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

static void output_geometry(void *data __attribute__((unused)),
                           struct wl_output *wl_output __attribute__((unused)),
                           int32_t x __attribute__((unused)),
                           int32_t y __attribute__((unused)),
                           int32_t physical_width __attribute__((unused)),
                           int32_t physical_height __attribute__((unused)),
                           int32_t subpixel __attribute__((unused)),
                           const char *make __attribute__((unused)),
                           const char *model __attribute__((unused)),
                           int32_t transform) {
    screen_info.transform = transform;
    screen_info.geometry_received = true;
    bongocat_log_debug("Output transform: %d", transform);
    screen_calculate_dimensions();
}

static void output_mode(void *data __attribute__((unused)),
                       struct wl_output *wl_output __attribute__((unused)),
                       uint32_t flags, int32_t width, int32_t height,
                       int32_t refresh __attribute__((unused))) {
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        screen_info.raw_width = width;
        screen_info.raw_height = height;
        screen_info.mode_received = true;
        bongocat_log_debug("Received raw screen mode: %dx%d", width, height);
        screen_calculate_dimensions();
    }
}

static void output_done(void *data __attribute__((unused)),
                       struct wl_output *wl_output __attribute__((unused))) {
    screen_calculate_dimensions();
    bongocat_log_debug("Output configuration complete");
}

static void output_scale(void *data __attribute__((unused)),
                        struct wl_output *wl_output __attribute__((unused)),
                        int32_t factor __attribute__((unused))) {
    // Scale not needed for our use case
}

static struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
};

// =============================================================================
// WAYLAND PROTOCOL REGISTRY
// =============================================================================

static void registry_global(void *data __attribute__((unused)), struct wl_registry *reg, 
                           uint32_t name, const char *iface, uint32_t ver __attribute__((unused))) {
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        compositor = (struct wl_compositor *)wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        shm = (struct wl_shm *)wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
        layer_shell = (struct zwlr_layer_shell_v1 *)wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
    } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        xdg_wm_base = (struct xdg_wm_base *)wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
        if (xdg_wm_base) {
            xdg_wm_base_add_listener(xdg_wm_base, &xdg_wm_base_listener, NULL);
        }
    } else if (strcmp(iface, zxdg_output_manager_v1_interface.name) == 0) {
        xdg_output_manager = wl_registry_bind(reg, name, &zxdg_output_manager_v1_interface, 3);
    } else if (strcmp(iface, wl_output_interface.name) == 0) {
        if (output_count < MAX_OUTPUTS) {
            outputs[output_count].name = name;
            outputs[output_count].wl_output = wl_registry_bind(reg, name, &wl_output_interface, 2);
            wl_output_add_listener(outputs[output_count].wl_output, &output_listener, NULL);
            output_count++;
        }
    } else if (strcmp(iface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
        fs_detector.manager = (struct zwlr_foreign_toplevel_manager_v1 *)
            wl_registry_bind(reg, name, &zwlr_foreign_toplevel_manager_v1_interface, 3);
        if (fs_detector.manager) {
            zwlr_foreign_toplevel_manager_v1_add_listener(fs_detector.manager, &fs_manager_listener, NULL);
            bongocat_log_info("Foreign toplevel manager bound - using Wayland protocol for fullscreen detection");
        }
    }
}

static void registry_remove(void *data __attribute__((unused)),
                           struct wl_registry *registry __attribute__((unused)),
                           uint32_t name __attribute__((unused))) {}

static struct wl_registry_listener reg_listener = {
    .global = registry_global,
    .global_remove = registry_remove
};

// =============================================================================
// MAIN WAYLAND INTERFACE IMPLEMENTATION
// =============================================================================

static bongocat_error_t wayland_setup_protocols(void) {
    struct wl_registry *registry = wl_display_get_registry(display);
    if (!registry) {
        bongocat_log_error("Failed to get Wayland registry");
        return BONGOCAT_ERROR_WAYLAND;
    }

    wl_registry_add_listener(registry, &reg_listener, NULL);
    wl_display_roundtrip(display);

    if (xdg_output_manager) {
        for (size_t i = 0; i < output_count; ++i) {
            outputs[i].xdg_output = zxdg_output_manager_v1_get_xdg_output(xdg_output_manager, outputs[i].wl_output);
            zxdg_output_v1_add_listener(outputs[i].xdg_output, &xdg_output_listener, &outputs[i]);
        }

        // Wait for all xdg_output events
        wl_display_roundtrip(display);
    }

    output = NULL;
    if (current_config->output_name) {
        for (size_t i = 0; i < output_count; ++i) {
            if (outputs[i].name_received &&
                strcmp(outputs[i].name_str, current_config->output_name) == 0) {
                output = outputs[i].wl_output;
                bongocat_log_info("Matched output: %s", outputs[i].name_str);
                break;
                }
        }

        if (!output) {
            bongocat_log_error("Could not find output named '%s', defaulting to first output",
                               current_config->output_name);
        }
    }

    // Fallback
    if (!output && output_count > 0) {
        output = outputs[0].wl_output;
        bongocat_log_warning("Falling back to first output");
    }

    if (!compositor || !shm || !layer_shell) {
        bongocat_log_error("Missing required Wayland protocols");
        wl_registry_destroy(registry);
        return BONGOCAT_ERROR_WAYLAND;
    }

    // Configure screen dimensions
    if (output) {
        wl_display_roundtrip(display);
        if (screen_info.screen_width > 0) {
            bongocat_log_info("Detected screen width: %d", screen_info.screen_width);
            current_config->screen_width = screen_info.screen_width;
        } else {
            bongocat_log_warning("Using default screen width: %d", DEFAULT_SCREEN_WIDTH);
            current_config->screen_width = DEFAULT_SCREEN_WIDTH;
        }
    } else {
        bongocat_log_warning("No output found, using default screen width: %d", DEFAULT_SCREEN_WIDTH);
        current_config->screen_width = DEFAULT_SCREEN_WIDTH;
    }

    wl_registry_destroy(registry);
    return BONGOCAT_SUCCESS;
}

static bongocat_error_t wayland_setup_surface(void) {
    surface = wl_compositor_create_surface(compositor);
    if (!surface) {
        bongocat_log_error("Failed to create surface");
        return BONGOCAT_ERROR_WAYLAND;
    }

    layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell, surface, output,
                                                      ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
                                                      "bongocat-overlay");

    if (!layer_surface) {
        bongocat_log_error("Failed to create layer surface");
        return BONGOCAT_ERROR_WAYLAND;
    }

    // Configure layer surface
    uint32_t anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    if (current_config->overlay_position == POSITION_TOP) {
        anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
    } else {
        anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
    }
    
    zwlr_layer_surface_v1_set_anchor(layer_surface, anchor);
    zwlr_layer_surface_v1_set_size(layer_surface, 0, current_config->bar_height);
    zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface,
                                                     ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, NULL);

    // Make surface click-through
    struct wl_region *input_region = wl_compositor_create_region(compositor);
    if (input_region) {
        wl_surface_set_input_region(surface, input_region);
        wl_region_destroy(input_region);
    }

    wl_surface_commit(surface);
    return BONGOCAT_SUCCESS;
}

static bongocat_error_t wayland_setup_buffer(void) {
    int size = current_config->screen_width * current_config->bar_height * 4;
    if (size <= 0) {
        bongocat_log_error("Invalid buffer size: %d", size);
        return BONGOCAT_ERROR_WAYLAND;
    }

    int fd = create_shm(size);
    if (fd < 0) {
        return BONGOCAT_ERROR_WAYLAND;
    }

    pixels = (uint8_t *)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        bongocat_log_error("Failed to map shared memory: %s", strerror(errno));
        close(fd);
        return BONGOCAT_ERROR_MEMORY;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    if (!pool) {
        bongocat_log_error("Failed to create shared memory pool");
        munmap(pixels, size);
        pixels = NULL;
        close(fd);
        return BONGOCAT_ERROR_WAYLAND;
    }

    buffer = wl_shm_pool_create_buffer(pool, 0, current_config->screen_width, 
                                      current_config->bar_height,
                                      current_config->screen_width * 4, 
                                      WL_SHM_FORMAT_ARGB8888);
    if (!buffer) {
        bongocat_log_error("Failed to create buffer");
        wl_shm_pool_destroy(pool);
        munmap(pixels, size);
        pixels = NULL;
        close(fd);
        return BONGOCAT_ERROR_WAYLAND;
    }

    wl_shm_pool_destroy(pool);
    close(fd);
    return BONGOCAT_SUCCESS;
}

bongocat_error_t wayland_init(config_t *config) {
    BONGOCAT_CHECK_NULL(config, BONGOCAT_ERROR_INVALID_PARAM);

    current_config = config;
    bongocat_log_info("Initializing Wayland connection");

    display = wl_display_connect(NULL);
    if (!display) {
        bongocat_log_error("Failed to connect to Wayland display");
        return BONGOCAT_ERROR_WAYLAND;
    }

    bongocat_error_t result;
    if ((result = wayland_setup_protocols()) != BONGOCAT_SUCCESS ||
        (result = wayland_setup_surface()) != BONGOCAT_SUCCESS ||
        (result = wayland_setup_buffer()) != BONGOCAT_SUCCESS) {
        wayland_cleanup();
        return result;
    }

    bongocat_log_info("Wayland initialization complete (%dx%d buffer)",
                     current_config->screen_width, current_config->bar_height);
    return BONGOCAT_SUCCESS;
}

bongocat_error_t wayland_run(volatile sig_atomic_t *running, int signal_fd, config_reload_callback_t config_reload_callback) {
    BONGOCAT_CHECK_NULL(running, BONGOCAT_ERROR_INVALID_PARAM);

    bongocat_log_info("Starting Wayland event loop");
    const int check_interval_ms = 100;

    while (*running && display) {
        // Periodic fullscreen check for fallback detection
        struct timeval now;
        gettimeofday(&now, NULL);
        long elapsed_ms = (now.tv_sec - fs_detector.last_check.tv_sec) * 1000 + 
                         (now.tv_usec - fs_detector.last_check.tv_usec) / 1000;
        
        if (elapsed_ms >= check_interval_ms) {
            bool new_state = fs_check_status();
            if (new_state != fullscreen_detected) {
                fs_update_state(new_state);
            }
            fs_detector.last_check = now;
        }

        // Handle Wayland and signal events
        struct pollfd pfds[2] = {
            { .fd = wl_display_get_fd(display), .events = POLLIN, },
            { .fd = signal_fd, .events = POLLIN },
        };
        
        while (wl_display_prepare_read(display) != 0) {
            if (wl_display_dispatch_pending(display) == -1) {
                bongocat_log_error("Failed to dispatch pending events");
                return BONGOCAT_ERROR_WAYLAND;
            }
        }
        
        int poll_result = poll(pfds, 2, 100);
        if (poll_result > 0) {
            // signal events
            if (pfds[1].revents & POLLIN) {
                struct signalfd_siginfo fdsi;
                ssize_t s = read(pfds[1].fd, &fdsi, sizeof(fdsi));
                if (s != sizeof(fdsi)) {
                    bongocat_log_error("Failed to read signal fd");
                } else {
                    switch (fdsi.ssi_signo) {
                        case SIGINT:
                        case SIGTERM:
                            bongocat_log_info("Received signal %d, shutting down gracefully", fdsi.ssi_signo);
                            *running = 0;
                            break;
                        case SIGCHLD:
                            while (waitpid(-1, NULL, WNOHANG) > 0);
                            break;
                        case SIGUSR2:
                            bongocat_log_info("Received SIGUSR2, reloading config");
                            if (config_reload_callback) {
                                config_reload_callback();
                            }
                            break;
                        default:
                            bongocat_log_warning("Received unexpected signal %d", fdsi.ssi_signo);
                            break;
                    }
                }
            }
            if (!*running) {
                wl_display_cancel_read(display);
                break;
            }


            // wayland events
            if (pfds[0].revents & POLLIN) {
                if (wl_display_read_events(display) == -1 ||
                    wl_display_dispatch_pending(display) == -1) {
                    bongocat_log_error("Failed to handle Wayland events");
                    *running = 0;
                    return BONGOCAT_ERROR_WAYLAND;
                }
            } else {
                wl_display_cancel_read(display);
            }
        } else if (poll_result == 0) {
            wl_display_cancel_read(display);
        } else {
            wl_display_cancel_read(display);
            if (errno != EINTR) {
                bongocat_log_error("Poll error: %s", strerror(errno));
                return BONGOCAT_ERROR_WAYLAND;
            }
        }

        wl_display_flush(display);
    }

    bongocat_log_info("Wayland event loop exited");
    return BONGOCAT_SUCCESS;
}

// =============================================================================
// PUBLIC API IMPLEMENTATION
// =============================================================================

int wayland_get_screen_width(void) {
    return screen_info.screen_width;
}

void wayland_update_config(config_t *config) {
    if (!config) {
        bongocat_log_error("Cannot update wayland config: config is NULL");
        return;
    }

    current_config = config;
    if (configured) {
        draw_bar();
    }
}

void wayland_cleanup(void) {
    bongocat_log_info("Cleaning up Wayland resources");

    // First destroy xdg_output objects
    for (size_t i = 0; i < output_count; ++i) {
        if (outputs[i].xdg_output) {
            bongocat_log_debug("Destroying xdg_output %zu", i);
            zxdg_output_v1_destroy(outputs[i].xdg_output);
            outputs[i].xdg_output = NULL;
        }
    }

    // Then destroy the manager
    if (xdg_output_manager) {
        bongocat_log_debug("Destroying xdg_output_manager");
        zxdg_output_manager_v1_destroy(xdg_output_manager);
        xdg_output_manager = NULL;
    }

    // Finally destroy wl_output objects
    for (size_t i = 0; i < output_count; ++i) {
        if (outputs[i].wl_output) {
            bongocat_log_debug("Destroying wl_output %zu", i);
            wl_output_destroy(outputs[i].wl_output);
            outputs[i].wl_output = NULL;
        }
    }
    
    output_count = 0;

    if (buffer) {
        wl_buffer_destroy(buffer);
        buffer = NULL;
    }

    if (pixels && current_config) {
        int size = current_config->screen_width * current_config->bar_height * 4;
        munmap(pixels, size);
        pixels = NULL;
    }

    if (layer_surface) {
        zwlr_layer_surface_v1_destroy(layer_surface);
        layer_surface = NULL;
    }

    if (surface) {
        wl_surface_destroy(surface);
        surface = NULL;
    }

    // Note: output is just a reference to one of the outputs[] entries
    // It will be destroyed when we destroy the outputs[] array above
    output = NULL;

    if (layer_shell) {
        zwlr_layer_shell_v1_destroy(layer_shell);
        layer_shell = NULL;
    }

    if (xdg_wm_base) {
        xdg_wm_base_destroy(xdg_wm_base);
        xdg_wm_base = NULL;
    }

    if (fs_detector.manager) {
        zwlr_foreign_toplevel_manager_v1_destroy(fs_detector.manager);
        fs_detector.manager = NULL;
    }

    if (shm) {
        wl_shm_destroy(shm);
        shm = NULL;
    }

    if (compositor) {
        wl_compositor_destroy(compositor);
        compositor = NULL;
    }

    if (display) {
        wl_display_disconnect(display);
        display = NULL;
    }

    // Reset state
    configured = false;
    fullscreen_detected = false;
    memset(&fs_detector, 0, sizeof(fs_detector));
    memset(&screen_info, 0, sizeof(screen_info));
    
    bongocat_log_debug("Wayland cleanup complete");
}

const char* wayland_get_current_layer_name(void) {
    return "OVERLAY";
}
