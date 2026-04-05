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

// Pre-scaled frame cache (avoids repeated scaling of constant source images)
typedef struct {
  uint8_t *data;  // Pre-scaled BGRA pixel data (NULL if not cached)
  int width;
  int height;
} cached_frame_t;

extern cached_frame_t anim_cached_frames[NUM_FRAMES];

void animation_cache_frames(int target_w, int target_h, int mirror_x,
                            int mirror_y, int enable_aa);
void animation_invalidate_cache(void);

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

// Blit scaled image to destination buffer (converts RGBA source to BGRA dest)
void blit_image_scaled(uint8_t *dest, int dest_w, int dest_h,
                       unsigned char *src, int src_w, int src_h, int offset_x,
                       int offset_y, int target_w, int target_h);

// Blit pre-converted cached frame (BGRA to BGRA, no channel swap)
void blit_cached_frame(uint8_t *dest, int dest_w, int dest_h,
                       const uint8_t *src, int src_w, int src_h, int offset_x,
                       int offset_y);

#endif  // ANIMATION_H