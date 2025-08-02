#ifndef ANIMATION_H
#define ANIMATION_H

#include "core/bongocat.h"
#include "config/config.h"
#include "utils/error.h"
#include "platform/wayland.h"
#include "context.h"

bongocat_error_t animation_init(animation_context_t* ctx, config_t *config);
bongocat_error_t animation_start(animation_context_t* ctx, input_context_t *input, wayland_context_t *wayland);
void animation_cleanup(animation_context_t* ctx);
void animation_trigger(input_context_t *input);

void blit_image_scaled(uint8_t *dest, int dest_w, int dest_h,
                      const unsigned char *src, int src_w, int src_h,
                      int offset_x, int offset_y, int target_w, int target_h);

void draw_rect(uint8_t *dest, int width, int height, int x, int y, 
               int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

#endif // ANIMATION_H