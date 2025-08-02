#define _POSIX_C_SOURCE 200809L
#include "wayland.h"
#include "animation.h"
#include "embedded_assets.h"
#include <assert.h>

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

void draw_bar(wayland_context_t* ctx, animation_context_t* anim) {
    if (!ctx->configured) {
        bongocat_log_debug("Surface not configured yet, skipping draw");
        return;
    }

    // Clear entire buffer with configurable transparency
    for (int i = 0; i < ctx->current_config->screen_width * ctx->current_config->bar_height * 4; i += 4) {
        ctx->pixels[i] = 0;       // B
        ctx->pixels[i + 1] = 0;   // G
        ctx->pixels[i + 2] = 0;   // R
        ctx->pixels[i + 3] = ctx->current_config->overlay_opacity; // A (configurable transparency)
    }

    pthread_mutex_lock(&anim->anim_lock);

    if (ctx->current_config->animation_index == BONGOCAT_ANIM_INDEX) {
        // Use configured cat height
        int cat_height = ctx->current_config->cat_height;
        int cat_width = (cat_height * 779) / 320;  // Maintain 954:393 ratio
        int cat_x = (ctx->current_config->screen_width - cat_width) / 2 + ctx->current_config->cat_x_offset;
        int cat_y = (ctx->current_config->bar_height - cat_height) / 2 + ctx->current_config->cat_y_offset;

        blit_image_scaled(ctx->pixels, ctx->current_config->screen_width, ctx->current_config->bar_height,
                          anim->anims[ctx->current_config->animation_index].frames[anim->anim_frame_index].pixels,
                          anim->anims[ctx->current_config->animation_index].frames[anim->anim_frame_index].width,
                          anim->anims[ctx->current_config->animation_index].frames[anim->anim_frame_index].height,
                          cat_x, cat_y, cat_width, cat_height);
    } else if (ctx->current_config->animation_index >= DM20_AGUMON_ANIM_INDEX) { // otherwise its digimon
        // Use configured cat height
        int cat_height = ctx->current_config->cat_height;
        int cat_width = (cat_height * anim->anims[ctx->current_config->animation_index].frames[anim->anim_frame_index].width) / anim->anims[ctx->current_config->animation_index].frames[anim->anim_frame_index].height;  // Maintain ratio
        int cat_x = (ctx->current_config->screen_width - cat_width) / 2 + ctx->current_config->cat_x_offset;
        int cat_y = (ctx->current_config->bar_height - cat_height) / 2 + ctx->current_config->cat_y_offset;

        blit_image_scaled(ctx->pixels, ctx->current_config->screen_width, ctx->current_config->bar_height,
                          anim->anims[ctx->current_config->animation_index].frames[anim->anim_frame_index].pixels,
                          anim->anims[ctx->current_config->animation_index].frames[anim->anim_frame_index].width,
                          anim->anims[ctx->current_config->animation_index].frames[anim->anim_frame_index].height,
                          cat_x, cat_y, cat_width, cat_height);
    }

    pthread_mutex_unlock(&anim->anim_lock);

    wl_surface_attach(ctx->surface, ctx->buffer, 0, 0);
    wl_surface_damage_buffer(ctx->surface, 0, 0, ctx->current_config->screen_width, ctx->current_config->bar_height);
    wl_surface_commit(ctx->surface);
    wl_display_flush(ctx->display);
}

/// @TODO: get rid of globals, bind variables into layer_listener
static wayland_context_t* g_layer_surface_configure_animation_ctx = NULL;
static animation_context_t* g_layer_surface_configure_animation = NULL;
static void layer_surface_configure(void *data __attribute__((unused)),
                                   struct zwlr_layer_surface_v1 *ls,
                                   uint32_t serial, uint32_t w, uint32_t h) {
    assert(g_layer_surface_configure_animation);
    bongocat_log_debug("Layer surface configured: %dx%d", w, h);
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    g_layer_surface_configure_animation_ctx->configured = true;
    draw_bar(g_layer_surface_configure_animation_ctx, g_layer_surface_configure_animation);
}

static struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_surface_configure,
    .closed = NULL,
};

static void calculate_effective_screen_dimensions(wayland_context_t* ctx) {
    if (!ctx->_wayland_mode_received || !ctx->_wayland_geometry_received) {
        return; // Wait for both mode and geometry
    }
    
    // Only calculate and log if we haven't already done so
    if (ctx->_wayland_screen_width > 0) {
        return; // Already calculated
    }
    
    // Check if the display is rotated (90° or 270°)
    // In these cases, width and height are swapped from the user's perspective
    // WL_OUTPUT_TRANSFORM values:
    // 0 = normal, 1 = 90°, 2 = 180°, 3 = 270°
    // 4 = flipped, 5 = flipped+90°, 6 = flipped+180°, 7 = flipped+270°
    bool is_rotated = (ctx->_wayland_transform == WL_OUTPUT_TRANSFORM_90 ||
                      ctx->_wayland_transform == WL_OUTPUT_TRANSFORM_270 ||
                      ctx->_wayland_transform == WL_OUTPUT_TRANSFORM_FLIPPED_90 ||
                      ctx->_wayland_transform == WL_OUTPUT_TRANSFORM_FLIPPED_270);
    
    if (is_rotated) {
        // For rotated displays, swap width and height to get the effective screen width
        ctx->_wayland_screen_width = ctx->_wayland_raw_height;
        ctx->_wayland_screen_height = ctx->_wayland_raw_width;
        bongocat_log_info("Detected rotated screen dimensions: %dx%d (transform: %d)", 
                         ctx->_wayland_raw_height, ctx->_wayland_raw_width, ctx->_wayland_transform);
    } else {
        ctx->_wayland_screen_width = ctx->_wayland_raw_width;
        ctx->_wayland_screen_height = ctx->_wayland_raw_height;
        bongocat_log_info("Detected screen dimensions: %dx%d (transform: %d)", 
                         ctx->_wayland_raw_width, ctx->_wayland_raw_height, ctx->_wayland_transform);
    }
}

/// @TODO: get rid of globals, bind variables
wayland_context_t* g_output_ctx = NULL;
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
    assert(g_output_ctx);
    g_output_ctx->_wayland_transform = transform;
    g_output_ctx->_wayland_geometry_received = true;
    bongocat_log_debug("Output transform: %d", transform);
    calculate_effective_screen_dimensions(g_output_ctx);
}

static void output_mode(void *data __attribute__((unused)),
                       struct wl_output *wl_output __attribute__((unused)),
                       uint32_t flags,
                       int32_t width,
                       int32_t height,
                       int32_t refresh __attribute__((unused))) {
    assert(g_output_ctx);
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        g_output_ctx->_wayland_raw_width = width;
        g_output_ctx->_wayland_raw_height = height;
        g_output_ctx->_wayland_mode_received = true;
        bongocat_log_debug("Received raw screen mode: %dx%d", width, height);
        calculate_effective_screen_dimensions(g_output_ctx);
    }
}

static void output_done(void *data __attribute__((unused)),
                       struct wl_output *wl_output __attribute__((unused))) {
    assert(g_output_ctx);
    // Output configuration is complete - ensure we have calculated dimensions
    calculate_effective_screen_dimensions(g_output_ctx);
    bongocat_log_debug("Output configuration complete");
}

static void output_scale(void *data __attribute__((unused)),
                        struct wl_output *wl_output __attribute__((unused)),
                        int32_t factor __attribute__((unused))) {
    // We don't need scale info for screen width detection
    assert(g_output_ctx);
}

static struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
};

static void registry_global(void *data __attribute__((unused)), struct wl_registry *reg, uint32_t name,
                           const char *iface, uint32_t ver __attribute__((unused))) {
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        g_output_ctx->compositor = (struct wl_compositor *)wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        g_output_ctx->shm = (struct wl_shm *)wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
        g_output_ctx->layer_shell = (struct zwlr_layer_shell_v1 *)wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
    } else if (strcmp(iface, wl_output_interface.name) == 0) {
        // Bind to the first output we find
        if (!g_output_ctx->output) {
            g_output_ctx->output = (struct wl_output *)wl_registry_bind(reg, name, &wl_output_interface, 2);
            wl_output_add_listener(g_output_ctx->output, &output_listener, NULL);
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

bongocat_error_t wayland_init(wayland_context_t* ctx, animation_context_t* anim, config_t *config) {
    BONGOCAT_CHECK_NULL(ctx, BONGOCAT_ERROR_INVALID_PARAM);
    BONGOCAT_CHECK_NULL(anim, BONGOCAT_ERROR_INVALID_PARAM);
    BONGOCAT_CHECK_NULL(config, BONGOCAT_ERROR_INVALID_PARAM);

    // Wayland globals
    ctx->configured = false;

    // Screen dimensions from Wayland output
    ctx->_wayland_screen_width = 0;
    ctx->_wayland_screen_height = 0;
    ctx->_wayland_transform = WL_OUTPUT_TRANSFORM_NORMAL;
    ctx->_wayland_raw_width = 0;
    ctx->_wayland_raw_height = 0;
    ctx->_wayland_mode_received = false;
    ctx->_wayland_geometry_received = false;

    bongocat_log_info("Initializing Wayland connection");

    ctx->current_config = config;

    ctx->display = wl_display_connect(NULL);
    if (!ctx->display) {
        bongocat_log_error("Failed to connect to Wayland display");
        return BONGOCAT_ERROR_WAYLAND;
    }
    bongocat_log_debug("Connected to Wayland display");

    struct wl_registry *registry = wl_display_get_registry(ctx->display);
    if (!registry) {
        bongocat_log_error("Failed to get Wayland registry");
        wl_display_disconnect(ctx->display);
        ctx->display = NULL;
        return BONGOCAT_ERROR_WAYLAND;
    }

    g_output_ctx = ctx;
    wl_registry_add_listener(registry, &reg_listener, NULL);
    wl_display_roundtrip(ctx->display);

    if (!ctx->compositor || !ctx->shm || !ctx->layer_shell) {
        bongocat_log_error("Missing required Wayland protocols (compositor=%p, shm=%p, layer_shell=%p)",
                          (void*)ctx->compositor, (void*)ctx->shm, (void*)ctx->layer_shell);
        wl_registry_destroy(registry);
        wl_display_disconnect(ctx->display);
        ctx->display = NULL;
        return BONGOCAT_ERROR_WAYLAND;
    }
    bongocat_log_debug("Got required Wayland protocols");

    // Wait for output information if we have an output
    if (ctx->output) {
        wl_display_roundtrip(ctx->display);
        if (ctx->_wayland_screen_width > 0) {
            bongocat_log_info("Detected screen width from Wayland: %d", ctx->_wayland_screen_width);
            config->screen_width = ctx->_wayland_screen_width;
        } else {
            bongocat_log_warning("Failed to detect screen width from Wayland, using default: %d", DEFAULT_SCREEN_WIDTH);
            config->screen_width = DEFAULT_SCREEN_WIDTH;
        }
    } else {
        bongocat_log_warning("No Wayland output found, using default screen width: %d", DEFAULT_SCREEN_WIDTH);
        config->screen_width = DEFAULT_SCREEN_WIDTH;
    }

    ctx->surface = wl_compositor_create_surface(ctx->compositor);
    if (!ctx->surface) {
        bongocat_log_error("Failed to create Wayland surface");
        wl_registry_destroy(registry);
        wl_display_disconnect(ctx->display);
        ctx->display = NULL;
        return BONGOCAT_ERROR_WAYLAND;
    }

    ctx->layer_surface = zwlr_layer_shell_v1_get_layer_surface(ctx->layer_shell, ctx->surface, NULL,
                                                          ZWLR_LAYER_SHELL_V1_LAYER_TOP, "bongocat-overlay");
    if (!ctx->layer_surface) {
        bongocat_log_error("Failed to create layer surface");
        wl_surface_destroy(ctx->surface);
        ctx->surface = NULL;
        wl_registry_destroy(registry);
        wl_display_disconnect(ctx->display);
        ctx->display = NULL;
        return BONGOCAT_ERROR_WAYLAND;
    }

    // Set anchor based on configured position
    uint32_t anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    if (config->overlay_position == POSITION_TOP) {
        anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
        bongocat_log_debug("Setting overlay position to top");
    } else if (config->overlay_position == POSITION_BOTTOM) {
        anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
        bongocat_log_debug("Setting overlay position to bottom");
    } else if (config->overlay_position == POSITION_TOP_LEFT) {
        anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
        bongocat_log_debug("Setting overlay position to top-left");
    } else if (config->overlay_position == POSITION_BOTTOM_LEFT) {
        anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
        bongocat_log_debug("Setting overlay position to left-bottom");
    } else if (config->overlay_position == POSITION_TOP_RIGHT) {
        anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT | ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
        bongocat_log_debug("Setting overlay position to top-right");
    } else if (config->overlay_position == POSITION_BOTTOM_RIGHT) {
        anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
        bongocat_log_debug("Setting overlay position to right-bottom");
    } else {
        anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
        bongocat_log_debug("Setting overlay position to bottom");
    }

    zwlr_layer_surface_v1_set_anchor(ctx->layer_surface, anchor);
    zwlr_layer_surface_v1_set_size(ctx->layer_surface, 0, config->bar_height);
    zwlr_layer_surface_v1_set_exclusive_zone(ctx->layer_surface, -1);
    zwlr_layer_surface_v1_set_margin(ctx->layer_surface, 0, 0, 0, 0);

    // Make the overlay click-through by setting keyboard interactivity to none
    zwlr_layer_surface_v1_set_keyboard_interactivity(ctx->layer_surface,
                                                     ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

    g_layer_surface_configure_animation = anim;
    g_layer_surface_configure_animation_ctx = ctx;
    zwlr_layer_surface_v1_add_listener(ctx->layer_surface, &layer_listener, NULL);

    // Create an empty input region to make the surface click-through
    struct wl_region *input_region = wl_compositor_create_region(ctx->compositor);
    if (input_region) {
        // Don't add any rectangles to the region, keeping it empty
        wl_surface_set_input_region(ctx->surface, input_region);
        wl_region_destroy(input_region);
        bongocat_log_debug("Set empty input region for click-through functionality");
    } else {
        bongocat_log_warning("Failed to create input region for click-through");
    }

    wl_surface_commit(ctx->surface);

    // Create shared memory buffer
    int size = config->screen_width * config->bar_height * 4;
    if (size <= 0) {
        bongocat_log_error("Invalid buffer size: %d", size);
        return BONGOCAT_ERROR_WAYLAND;
    }

    int fd = create_shm(size);
    if (fd < 0) {
        bongocat_log_error("Failed to create shared memory");
        return BONGOCAT_ERROR_WAYLAND;
    }

    ctx->pixels = (uint8_t *)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ctx->pixels == MAP_FAILED) {
        bongocat_log_error("Failed to map shared memory: %s", strerror(errno));
        close(fd);
        return BONGOCAT_ERROR_MEMORY;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(ctx->shm, fd, size);
    if (!pool) {
        bongocat_log_error("Failed to create shared memory pool");
        munmap(ctx->pixels, size);
        ctx->pixels = NULL;
        close(fd);
        return BONGOCAT_ERROR_WAYLAND;
    }

    ctx->buffer = wl_shm_pool_create_buffer(pool, 0, config->screen_width, config->bar_height,
                                      config->screen_width * 4, WL_SHM_FORMAT_ARGB8888);
    if (!ctx->buffer) {
        bongocat_log_error("Failed to create buffer");
        wl_shm_pool_destroy(pool);
        munmap(ctx->pixels, size);
        ctx->pixels = NULL;
        close(fd);
        return BONGOCAT_ERROR_WAYLAND;
    }

    wl_shm_pool_destroy(pool);
    close(fd);
    wl_registry_destroy(registry);

    bongocat_log_info("Wayland initialization complete (%dx%d buffer)",
                     config->screen_width, config->bar_height);
    return BONGOCAT_SUCCESS;
}

bongocat_error_t wayland_run(wayland_context_t* ctx, volatile sig_atomic_t *running) {
    BONGOCAT_CHECK_NULL(running, BONGOCAT_ERROR_INVALID_PARAM);

    bongocat_log_info("Starting Wayland event loop");

    while (*running && ctx->display) {
        int ret = wl_display_dispatch(ctx->display);
        if (ret == -1) {
            int err = wl_display_get_error(ctx->display);
            if (err == EPROTO) {
                bongocat_log_error("Wayland protocol error");
                *running = 0;
                return BONGOCAT_ERROR_WAYLAND;
            } else if (err == EPIPE) {
                bongocat_log_warning("Wayland display connection lost");
                *running = 0;
                return BONGOCAT_SUCCESS; // Graceful shutdown
            } else {
                bongocat_log_error("Wayland dispatch error: %s", strerror(err));
                *running = 0;
                return BONGOCAT_ERROR_WAYLAND;
            }
        }

        // Flush any pending requests
        if (wl_display_flush(ctx->display) == -1) {
            bongocat_log_warning("Failed to flush Wayland display");
        }
    }
    *running = 0;

    bongocat_log_info("Wayland event loop exited");
    return BONGOCAT_SUCCESS;
}

int wayland_get_screen_width(wayland_context_t* ctx) {
    return ctx->_wayland_screen_width;
}

void wayland_cleanup(wayland_context_t* ctx) {
    bongocat_log_info("Cleaning up Wayland resources");

    if (ctx->buffer) {
        wl_buffer_destroy(ctx->buffer);
        ctx->buffer = NULL;
    }

    if (ctx->pixels) {
        // Calculate buffer size for unmapping
        if (ctx->current_config) {
            int size = ctx->current_config->screen_width * ctx->current_config->bar_height * 4;
            munmap(ctx->pixels, size);
        }
        ctx->pixels = NULL;
    }

    if (ctx->layer_surface) {
        zwlr_layer_surface_v1_destroy(ctx->layer_surface);
        ctx->layer_surface = NULL;
    }

    if (ctx->surface) {
        wl_surface_destroy(ctx->surface);
        ctx->surface = NULL;
    }

    if (ctx->output) {
        wl_output_destroy(ctx->output);
        ctx->output = NULL;
    }

    if (ctx->layer_shell) {
        zwlr_layer_shell_v1_destroy(ctx->layer_shell);
        ctx->layer_shell = NULL;
    }

    if (ctx->shm) {
        wl_shm_destroy(ctx->shm);
        ctx->shm = NULL;
    }

    if (ctx->compositor) {
        wl_compositor_destroy(ctx->compositor);
        ctx->compositor = NULL;
    }

    if (ctx->display) {
        wl_display_disconnect(ctx->display);
        ctx->display = NULL;
    }

    ctx->configured = false;
    ctx->_wayland_screen_width = 0;
    ctx->_wayland_screen_height = 0;
    ctx->_wayland_transform = WL_OUTPUT_TRANSFORM_NORMAL;
    ctx->_wayland_raw_width = 0;
    ctx->_wayland_raw_height = 0;
    ctx->_wayland_mode_received = false;
    ctx->_wayland_geometry_received = false;
    ctx->current_config = NULL;
    bongocat_log_debug("Wayland cleanup complete");
}

void wayland_update_config(wayland_context_t* ctx, config_t *config, animation_context_t* anim) {
    if (!config) {
        bongocat_log_error("Cannot update wayland config: config is NULL");
        return;
    }

    bongocat_log_info("Updating wayland config");
    ctx->current_config = config;

    // Trigger a redraw with the new config
    if (ctx->configured) {
        draw_bar(ctx, anim);
        bongocat_log_info("Wayland config updated and redrawn");
    }
}
