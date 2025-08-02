#ifndef ANIMATION_H
#define ANIMATION_H

#include "bongocat.h"
#include "config.h"
#include "error.h"
#include "wayland.h"

extern const sprite_sheet_frame_t empty_sprite_sheet_frame;

bongocat_error_t animation_init(animation_context_t* ctx, const config_t *config);
bongocat_error_t animation_start(animation_context_t* ctx, input_context_t *input, wayland_context_t* wayland, config_t *config);
void animation_cleanup(animation_context_t* ctx);
void animation_trigger(input_context_t *input);

void blit_image_scaled(uint8_t *dest, int dest_w, int dest_h,
                      const unsigned char *src, int src_w, int src_h,
                      int offset_x, int offset_y, int target_w, int target_h);

void draw_rect(uint8_t *dest, int width, int height, int x, int y, 
               int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

sprite_sheet_frame_t* load_sprite_sheet_from_memory(const uint8_t* sprite_data, int sprite_size,
                                                    int frame_columns, int frame_rows, int* out_frame_count,
                                                    int padding_x, int padding_y, bool invert_color);
void free_frames(sprite_sheet_frame_t* frames, int frame_count);

#endif // ANIMATION_H