#ifndef ANIMATION_H
#define ANIMATION_H

#include "config/config.h"
#include "core/bongocat.h"
#include "utils/error.h"

#include <pthread.h>
#include <stdint.h>

// =============================================================================
// ANIMATION STATE
// =============================================================================

// Frame images and dimensions
extern unsigned char *anim_imgs[NUM_FRAMES];
extern int anim_width[NUM_FRAMES];
extern int anim_height[NUM_FRAMES];

// Current frame and synchronization
extern int anim_index;
extern pthread_mutex_t anim_lock;

// =============================================================================
// ANIMATION LIFECYCLE
// =============================================================================

// Initialize animation system - must be checked
BONGOCAT_NODISCARD bongocat_error_t animation_init(config_t *config);

// Start animation thread - must be checked
BONGOCAT_NODISCARD bongocat_error_t animation_start(void);

// Cleanup animation resources
void animation_cleanup(void);

// Trigger key press animation
void animation_trigger(void);

// =============================================================================
// RENDERING UTILITIES
// =============================================================================

// Blit scaled image to destination buffer
void blit_image_scaled(uint8_t *dest, int dest_w, int dest_h,
                       unsigned char *src, int src_w, int src_h, int offset_x,
                       int offset_y, int target_w, int target_h);

// Draw filled rectangle
void draw_rect(uint8_t *dest, int width, int height, int x, int y, int w, int h,
               uint8_t r, uint8_t g, uint8_t b, uint8_t a);

#endif  // ANIMATION_H