#define _POSIX_C_SOURCE 200809L
#include "config/config.h"

#include <ctype.h>

#include "utils/error.h"
#include "utils/memory.h"
#include "graphics/context.h"
#include "../../include/graphics/embedded_assets/bongocat.h"
#include "graphics/embedded_assets.h"
#include <pthread.h>
#include <limits.h>

// =============================================================================
// CONFIGURATION CONSTANTS AND VALIDATION RANGES
// =============================================================================

#define MIN_CAT_HEIGHT 10
#define MAX_CAT_HEIGHT 200
#define MIN_OVERLAY_HEIGHT 20
#define MAX_OVERLAY_HEIGHT 300
#define MIN_FPS 1
#define MAX_FPS 120
#define MIN_DURATION 10
#define MAX_DURATION 5000
#define MAX_INTERVAL 3600

// =============================================================================
// GLOBAL STATE FOR DEVICE MANAGEMENT
// =============================================================================

static char **config_keyboard_devices = NULL;
static int config_num_devices = 0;

// =============================================================================
// CONFIGURATION VALIDATION MODULE
// =============================================================================

static void config_clamp_int(int *value, int min, int max, const char *name) {
    if (*value < min || *value > max) {
        bongocat_log_warning("%s %d out of range [%d-%d], clamping", name, *value, min, max);
        *value = (*value < min) ? min : max;
    }
}

static void config_validate_dimensions(config_t *config) {
    config_clamp_int(&config->cat_height, MIN_CAT_HEIGHT, MAX_CAT_HEIGHT, "cat_height");
    config_clamp_int(&config->overlay_height, MIN_OVERLAY_HEIGHT, MAX_OVERLAY_HEIGHT, "overlay_height");
}

static void config_validate_timing(config_t *config) {
    config_clamp_int(&config->fps, MIN_FPS, MAX_FPS, "fps");
    config_clamp_int(&config->keypress_duration, MIN_DURATION, MAX_DURATION, "keypress_duration");
    config_clamp_int(&config->test_animation_duration, MIN_DURATION, MAX_DURATION, "test_animation_duration");
    
    // Validate interval (0 is allowed to disable)
    if (config->test_animation_interval < 0 || config->test_animation_interval > MAX_INTERVAL) {
        bongocat_log_warning("test_animation_interval %d out of range [0-%d], clamping", 
                           config->test_animation_interval, MAX_INTERVAL);
        config->test_animation_interval = (config->test_animation_interval < 0) ? 0 : MAX_INTERVAL;
    }
}

static void config_validate_appearance(config_t *config) {
    // Validate opacity
    config_clamp_int(&config->overlay_opacity, 0, 255, "overlay_opacity");

    // Validate animation index
    if (config->animation_index < 0 || config->animation_index >= TOTAL_ANIMATIONS) {
        bongocat_log_warning("animation_index %d out of range [0-%d], resetting to 0",
                           config->animation_index, TOTAL_ANIMATIONS - 1);
        config->animation_index = 0;
    }

    // Validate idle frame
    if (config->animation_index == BONGOCAT_ANIM_INDEX) {
        if (config->idle_frame < 0 || config->idle_frame >= BONGOCAT_NUM_FRAMES) {
            bongocat_log_warning("idle_frame %d out of range [0-%d], resetting to 0",
                               config->idle_frame, BONGOCAT_NUM_FRAMES - 1);
            config->idle_frame = 0;
        }
    } else {
        // digimon animation
        if (config->idle_frame < 0 || config->idle_frame >= MAX_DIGIMON_FRAMES) {
            bongocat_log_warning("idle_frame %d out of range [0-%d], resetting to 0",
                               config->idle_frame, MAX_DIGIMON_FRAMES - 1);
            config->idle_frame = 0;
        }
    }
}

static void config_validate_enums(config_t *config) {
    // Validate layer
    if (config->layer != LAYER_TOP && config->layer != LAYER_OVERLAY) {
        bongocat_log_warning("Invalid layer %d, resetting to top", config->layer);
        config->layer = LAYER_TOP;
    }
    
    // Validate overlay_position
    if (config->overlay_position != POSITION_TOP && config->overlay_position != POSITION_BOTTOM) {
        bongocat_log_warning("Invalid overlay_position %d, resetting to top", config->overlay_position);
        config->overlay_position = POSITION_TOP;
    }
}

static void config_validate_positioning(config_t *config) {
    // Validate cat positioning doesn't go off-screen
    if (abs(config->cat_x_offset) > config->screen_width) {
        bongocat_log_warning("cat_x_offset %d may position cat off-screen (screen width: %d)", 
                           config->cat_x_offset, config->screen_width);
    }
}

static bongocat_error_t config_validate(config_t *config) {
    BONGOCAT_CHECK_NULL(config, BONGOCAT_ERROR_INVALID_PARAM);
    
    config_validate_dimensions(config);
    config_validate_timing(config);
    config_validate_appearance(config);
    config_validate_enums(config);
    config_validate_positioning(config);
    
    // Normalize boolean values
    config->enable_debug = config->enable_debug ? 1 : 0;
    config->invert_color = config->invert_color ? 1 : 0;
    
    return BONGOCAT_SUCCESS;
}

// =============================================================================
// DEVICE MANAGEMENT MODULE
// =============================================================================

static bongocat_error_t config_add_keyboard_device(config_t *config, const char *device_path) {
    // Reallocate device array
    config_keyboard_devices = realloc(config_keyboard_devices, 
                                     (config_num_devices + 1) * sizeof(char*));
    if (!config_keyboard_devices) {
        bongocat_log_error("Failed to allocate memory for keyboard_devices");
        return BONGOCAT_ERROR_MEMORY;
    }
    
    size_t path_len = strlen(device_path);
    config_keyboard_devices[config_num_devices] = BONGOCAT_MALLOC(path_len + 1);
    if (!config_keyboard_devices[config_num_devices]) {
        bongocat_log_error("Failed to allocate memory for keyboard_device entry");
        return BONGOCAT_ERROR_MEMORY;
    }
    
    strncpy(config_keyboard_devices[config_num_devices], device_path, path_len);
    config_keyboard_devices[config_num_devices][path_len] = '\0';
    config_num_devices++;
    
    config->keyboard_devices = config_keyboard_devices;
    config->num_keyboard_devices = config_num_devices;
    
    return BONGOCAT_SUCCESS;
}

static void config_cleanup_devices(void) {
    if (config_keyboard_devices) {
        for (int i = 0; i < config_num_devices; i++) {
            BONGOCAT_SAFE_FREE(config_keyboard_devices[i]);
        }
        BONGOCAT_SAFE_FREE(config_keyboard_devices);
        config_keyboard_devices = NULL;
        config_num_devices = 0;
    }
}

// =============================================================================
// CONFIGURATION PARSING MODULE
// =============================================================================

static char* config_trim_key(char *key) {
    char *key_start = key;
    while (*key_start == ' ' || *key_start == '\t') key_start++;
    
    char *key_end = key_start + strlen(key_start) - 1;
    while (key_end > key_start && (*key_end == ' ' || *key_end == '\t')) {
        *key_end = '\0';
        key_end--;
    }
    
    return key_start;
}

static bongocat_error_t config_parse_integer_key(config_t *config, const char *key, const char *value) {
    int int_value = (int)strtol(value, NULL, 10);
    
    if (strcmp(key, "cat_x_offset") == 0) {
        config->cat_x_offset = int_value;
    } else if (strcmp(key, "cat_y_offset") == 0) {
        config->cat_y_offset = int_value;
    } else if (strcmp(key, "cat_height") == 0) {
        config->cat_height = int_value;
    } else if (strcmp(key, "overlay_height") == 0) {
        config->overlay_height = int_value;
    } else if (strcmp(key, "idle_frame") == 0) {
        config->idle_frame = int_value;
    } else if (strcmp(key, "keypress_duration") == 0) {
        config->keypress_duration = int_value;
    } else if (strcmp(key, "test_animation_duration") == 0) {
        config->test_animation_duration = int_value;
    } else if (strcmp(key, "test_animation_interval") == 0) {
        config->test_animation_interval = int_value;
    } else if (strcmp(key, "fps") == 0) {
        config->fps = int_value;
    } else if (strcmp(key, "overlay_opacity") == 0) {
        config->overlay_opacity = int_value;
    } else if (strcmp(key, "enable_debug") == 0) {
        config->enable_debug = int_value;
    } else if (strcmp(key, "animation_index") == 0) {
        config->animation_index = int_value;
    } else if (strcmp(key, "invert_color") == 0) {
        config->invert_color = int_value;
    } else if (strcmp(key, "padding_x") == 0) {
        config->padding_x = int_value;
    } else if (strcmp(key, "padding_y") == 0) {
        config->padding_y = int_value;
    } else {
        return BONGOCAT_ERROR_INVALID_PARAM; // Unknown key
    }
    
    return BONGOCAT_SUCCESS;
}

static bongocat_error_t config_parse_enum_key(config_t *config, const char *key, const char *value) {
    if (strcmp(key, "layer") == 0) {
        if (strcmp(value, "top") == 0) {
            config->layer = LAYER_TOP;
        } else if (strcmp(value, "overlay") == 0) {
            config->layer = LAYER_OVERLAY;
        } else {
            bongocat_log_warning("Invalid layer '%s', using 'top'", value);
            config->layer = LAYER_TOP;
        }
    } else if (strcmp(key, "overlay_position") == 0) {
        if (strcmp(value, "top") == 0) {
            config->overlay_position = POSITION_TOP;
        } else if (strcmp(value, "bottom") == 0) {
            config->overlay_position = POSITION_BOTTOM;
        } else {
            bongocat_log_warning("Invalid overlay_position '%s', using 'top'", value);
            config->overlay_position = POSITION_TOP;
        }
    } else if (strcmp(key, "animation_name") == 0) {
        char lower_value[256] = {0};
        memset(lower_value, 0, sizeof(lower_value));
        for(size_t i = 0; i < strlen(value); i++) {
            lower_value[i] = (char)tolower(value[i]);
        }

        config->animation_index = 0;
        if (strcmp(lower_value, "bongocat") == 0) {
            config->animation_index = BONGOCAT_ANIM_INDEX;
        }

        #ifdef FEATURE_INCLUDE_DM_EMBEDDED_ASSETS
        if (strcmp(lower_value, "agumon") == 0 || strcmp(lower_value, "dm:agumon") == 0) {
            config->animation_index = DM_AGUMON_ANIM_INDEX;
        }
        #endif

        if (config->animation_index < 0) {
            bongocat_log_warning("Invalid overlay_position '%s', using 'bongocat'", value);
            config->animation_index = BONGOCAT_ANIM_INDEX;
        }
    }else {
        return BONGOCAT_ERROR_INVALID_PARAM; // Unknown key
    }
    
    return BONGOCAT_SUCCESS;
}

static bongocat_error_t config_parse_key_value(config_t *config, const char *key, const char *value) {
    // Try integer keys first
    if (config_parse_integer_key(config, key, value) == BONGOCAT_SUCCESS) {
        return BONGOCAT_SUCCESS;
    }
    
    // Try enum keys
    if (config_parse_enum_key(config, key, value) == BONGOCAT_SUCCESS) {
        return BONGOCAT_SUCCESS;
    }
    
    // Handle device keys
    if (strcmp(key, "keyboard_device") == 0 || strcmp(key, "keyboard_devices") == 0) {
        return config_add_keyboard_device(config, value);
    }
    
    // Unknown key
    return BONGOCAT_ERROR_INVALID_PARAM;
}

static bool config_is_comment_or_empty(const char *line) {
    return (line[0] == '#' || line[0] == '\0' || strspn(line, " \t") == strlen(line));
}

static bongocat_error_t config_parse_file(config_t *config, const char *config_file_path) {
    BONGOCAT_CHECK_NULL(config, BONGOCAT_ERROR_INVALID_PARAM);
    
    const char *file_path = config_file_path ? config_file_path : "bongocat.conf";
    
    FILE *file = fopen(file_path, "r");
    if (!file) {
        bongocat_log_info("Config file '%s' not found, using defaults", file_path);
        return BONGOCAT_SUCCESS;
    }
    
    char line[512];
    char key[256], value[256];
    int line_number = 0;
    bongocat_error_t result = BONGOCAT_SUCCESS;
    
    while (fgets(line, sizeof(line), file)) {
        line_number++;
        
        // Remove trailing newline
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }
        
        // Skip comments and empty lines
        if (config_is_comment_or_empty(line)) {
            continue;
        }
        
        // Parse key=value pairs
        if (sscanf(line, " %255[^=] = %255s", key, value) == 2) {
            char *trimmed_key = config_trim_key(key);
            
            bongocat_error_t parse_result = config_parse_key_value(config, trimmed_key, value);
            if (parse_result == BONGOCAT_ERROR_INVALID_PARAM) {
                bongocat_log_warning("Unknown configuration key '%s' at line %d", trimmed_key, line_number);
            } else if (parse_result != BONGOCAT_SUCCESS) {
                result = parse_result;
                break;
            }
        } else if (strlen(line) > 0) {
            bongocat_log_warning("Invalid configuration line %d: %s", line_number, line);
        }
    }
    
    fclose(file);
    
    if (result == BONGOCAT_SUCCESS) {
        bongocat_log_info("Loaded configuration from %s", file_path);
    }
    
    return result;
}

// =============================================================================
// DEFAULT CONFIGURATION MODULE
// =============================================================================

static void config_set_defaults(config_t *config) {
    *config = (config_t) {
        .screen_width = DEFAULT_SCREEN_WIDTH,  // Will be updated by Wayland detection
        .bar_height = DEFAULT_BAR_HEIGHT,
        /*
        .asset_paths = {
            "assets/bongo-cat-both-up.png",
            "assets/bongo-cat-left-down.png",
            "assets/bongo-cat-right-down.png"
        },
        */
        .keyboard_devices = NULL,
        .num_keyboard_devices = 0,
        .cat_x_offset = 100,
        .cat_y_offset = 10,
        .cat_height = 40,
        .overlay_height = 50,
        .idle_frame = 0,
        .keypress_duration = 100,
        .test_animation_duration = 200,
        .test_animation_interval = 3,
        .fps = 60,
        .overlay_opacity = 150,
        .enable_debug = 1,
        .layer = LAYER_TOP,  // Default to TOP for broader compatibility
        .overlay_position = POSITION_TOP
    };
}

static bongocat_error_t config_set_default_devices(config_t *config) {
    if (config_num_devices == 0) {
        const char *default_device = "/dev/input/event4";
        return config_add_keyboard_device(config, default_device);
    }
    return BONGOCAT_SUCCESS;
}

static void config_finalize(config_t *config) {
    // Update bar_height from config
    config->bar_height = config->overlay_height;
    
    // Initialize error system with debug setting
    bongocat_error_init(config->enable_debug);
}

static void config_log_summary(const config_t *config) {
    bongocat_log_debug("Configuration loaded successfully");
    bongocat_log_debug("  Screen: %dx%d", config->screen_width, config->bar_height);
    if (config->animation_index == BONGOCAT_ANIM_INDEX) {
        bongocat_log_debug("  Cat: %dx%d at offset (%d,%d)",
                          config->cat_height, (config->cat_height * 954) / 393,
                          config->cat_x_offset, config->cat_y_offset);

    } else {
        bongocat_log_debug("  Digimon: %02d at offset (%d,%d)",
                          config->animation_index,
                          config->cat_x_offset, config->cat_y_offset);
    }
    bongocat_log_debug("  FPS: %d, Opacity: %d", config->fps, config->overlay_opacity);
    bongocat_log_debug("  Position: %s", config->overlay_position == POSITION_TOP ? "top" : "bottom");
    bongocat_log_debug("  Layer: %s", config->layer == LAYER_TOP ? "top" : "overlay");
}

// =============================================================================
// PUBLIC API IMPLEMENTATION
// =============================================================================

bongocat_error_t load_config(config_t *config, const char *config_file_path) {
    BONGOCAT_CHECK_NULL(config, BONGOCAT_ERROR_INVALID_PARAM);
    
    // Clear existing keyboard devices to prevent accumulation during reloads
    config_cleanup_devices();
    
    // Initialize with defaults
    config_set_defaults(config);
    
    // Set default keyboard device if none specified
    bongocat_error_t result = config_set_default_devices(config);
    if (result != BONGOCAT_SUCCESS) {
        bongocat_log_error("Failed to set default keyboard devices: %s", bongocat_error_string(result));
        return result;
    }
    
    // Parse config file and override defaults
    result = config_parse_file(config, config_file_path);
    if (result != BONGOCAT_SUCCESS) {
        bongocat_log_error("Failed to parse configuration file: %s", bongocat_error_string(result));
        return result;
    }
    
    // Validate and sanitize configuration
    result = config_validate(config);
    if (result != BONGOCAT_SUCCESS) {
        bongocat_log_error("Configuration validation failed: %s", bongocat_error_string(result));
        return result;
    }
    
    // Finalize configuration
    config_finalize(config);
    
    // Log configuration summary
    config_log_summary(config);
    
    return BONGOCAT_SUCCESS;
}

void config_cleanup(config_t *config) {
    config_cleanup_devices();
    config_set_defaults(config);
}

int get_screen_width(void) {
    // This function is now only used for initial config loading
    // The actual screen width detection happens in wayland_init
    return DEFAULT_SCREEN_WIDTH;
}
