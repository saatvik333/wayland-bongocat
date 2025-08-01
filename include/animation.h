#ifndef ANIMATION_H
#define ANIMATION_H

#include "bongocat.h"
#include "config.h"
#include "error.h"

#define THRESHOLD_ALPHA 127

#define DIGIMON_FRAME_IDLE1 0
#define DIGIMON_FRAME_IDLE2 1
#define DIGIMON_FRAME_ANGRY 2 // Angry/Refuse or Hit (Fallback), Eat Frame Fallback
#define DIGIMON_FRAME_DOWN1 3 // Sleep/Discipline Fallback
#define DIGIMON_FRAME_HAPPY 4
#define DIGIMON_FRAME_EAT1 5
#define DIGIMON_FRAME_SLEEP1 6
#define DIGIMON_FRAME_REFUSE 7
#define DIGIMON_FRAME_SAD 8

#define DIGIMON_FRAME_DOWN2 9
#define DIGIMON_FRAME_EAT2 10
#define DIGIMON_FRAME_SLEEP2 11
#define DIGIMON_FRAME_ATTACK 12

//#define DIGIMON_FRAME_MOVEMENT1 13
//#define DIGIMON_FRAME_MOVEMENT2 14

typedef struct {
    int width;
    int height;
    int channels;
    uint8_t* pixels;
} sprite_sheet_frame_t;

typedef struct {
    sprite_sheet_frame_t idle_1;
    sprite_sheet_frame_t idle_2;
    sprite_sheet_frame_t angry;
    sprite_sheet_frame_t down1;
    sprite_sheet_frame_t happy;
    sprite_sheet_frame_t eat1;
    sprite_sheet_frame_t sleep1;
    sprite_sheet_frame_t refuse;
    sprite_sheet_frame_t sad;

    // optional
    sprite_sheet_frame_t down_2;
    sprite_sheet_frame_t eat_2;
    sprite_sheet_frame_t sleep_2;
    sprite_sheet_frame_t attack;

    // extra frames
    //sprite_sheet_frame_t movement_1;
    //sprite_sheet_frame_t movement_2;
} digimon_animation_t;

typedef struct {
    sprite_sheet_frame_t both_up;
    sprite_sheet_frame_t left_down;
    sprite_sheet_frame_t right_down;
    sprite_sheet_frame_t both_down;

    sprite_sheet_frame_t _placeholder[MAX_DIGIMON_FRAMES-4];
} bongocat_animation_t;

typedef union {
    bongocat_animation_t bongocat;
    digimon_animation_t digimon;
    sprite_sheet_frame_t frames[MAX_DIGIMON_FRAMES];
} animation_t;


extern sprite_sheet_frame_t empty_sprite_sheet_frame;
extern animation_t anims[TOTAL_ANIMATIONS];
extern int anim_frame_index;
extern pthread_mutex_t anim_lock;

bongocat_error_t animation_init(config_t *config);
bongocat_error_t animation_start(void);
void animation_cleanup(void);
void animation_trigger(void);

void blit_image_scaled(uint8_t *dest, int dest_w, int dest_h,
                      unsigned char *src, int src_w, int src_h,
                      int offset_x, int offset_y, int target_w, int target_h);

void draw_rect(uint8_t *dest, int width, int height, int x, int y, 
               int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

sprite_sheet_frame_t* load_sprite_sheet_from_memory(const uint8_t* sprite_data, int sprite_size,
                                                    int frame_columns, int frame_rows, int* out_frame_count,
                                                    int padding_x, int padding_y, bool invert_color);
void free_frames(sprite_sheet_frame_t* frames, int frame_count);

#endif // ANIMATION_H