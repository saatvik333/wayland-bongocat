#ifndef BONGOCAT_ANIMATION_CONTEXT_H
#define BONGOCAT_ANIMATION_CONTEXT_H

#include "embedded_assets.h"
#include "core/bongocat.h"
#include "config/config.h"
#include <stdatomic.h>
#include <stdbool.h>

// both-up, left-down, right-down, both-down
#define BONGOCAT_NUM_FRAMES 4

// Idle 1, Idle 2, Angry, Down1, Happy, Eat1, Sleep1, Refuse, Down2 ~~, Eat2, Sleep2, Attack~~
// both-up, left-down, right-down, both-down, ...
#define MAX_NUM_FRAMES 15
#define MAX_DIGIMON_FRAMES 15


typedef struct {
    int width;
    int height;
    int channels;
    uint8_t *pixels;
} animation_frame_t;

typedef struct {
    animation_frame_t idle_1;
    animation_frame_t idle_2;
    animation_frame_t angry;
    animation_frame_t down1;
    animation_frame_t happy;
    animation_frame_t eat1;
    animation_frame_t sleep1;
    animation_frame_t refuse;
    animation_frame_t sad;

    // optional
    animation_frame_t down_2;
    animation_frame_t eat_2;
    animation_frame_t sleep_2;
    animation_frame_t attack;

    // extra frames
    animation_frame_t movement_1;
    animation_frame_t movement_2;
} digimon_animation_t;

typedef struct {
    animation_frame_t both_up;
    animation_frame_t left_down;
    animation_frame_t right_down;
    animation_frame_t both_down;

    animation_frame_t _placeholder[MAX_DIGIMON_FRAMES-4];
} bongocat_animation_t;

static_assert(sizeof(digimon_animation_t) == sizeof(bongocat_animation_t));
typedef union {
    bongocat_animation_t bongocat;
    digimon_animation_t digimon;
    animation_frame_t frames[MAX_NUM_FRAMES];
} animation_t;
static_assert(sizeof(animation_frame_t[MAX_NUM_FRAMES]) == sizeof(bongocat_animation_t));
static_assert(sizeof(animation_frame_t[MAX_NUM_FRAMES]) == sizeof(digimon_animation_t));


typedef struct {
    // Animation frame data
    animation_t anims[TOTAL_ANIMATIONS];
    int anim_index;
    int anim_frame_index;
    pthread_mutex_t anim_lock;

    // Animation system state
    config_t *_current_config;
    pthread_t _anim_thread;
    atomic_bool _animation_running;
} animation_context_t;

#endif //ANIMATION_CONTEXT_H
