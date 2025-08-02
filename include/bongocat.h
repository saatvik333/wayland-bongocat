#ifndef BONGOCAT_H
#define BONGOCAT_H

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <linux/input.h>
#include <sys/inotify.h>
#include <signal.h>

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <wayland-client.h>

#include "../lib/stb_image.h"

// Version
#define BONGOCAT_VERSION "1.2.2"

// Common constants
#define BONGOCAT_NUM_FRAMES 4
#define DEFAULT_SCREEN_WIDTH 1920
#define DEFAULT_BAR_HEIGHT 40

// Idle 1, Idle 2, Angry, Down1, Happy, Eat1, Sleep1, Refuse, Down2 ~~, Eat2, Sleep2, Attack~~
// both-up, left-down, right-down, both-down, ...
#define MAX_NUM_FRAMES 12
#define MAX_DIGIMON_FRAMES 12

// Animations
#define TOTAL_DIGIMON_ANIMATIONS 1

// bongocat + digimons
#define TOTAL_ANIMATIONS (1+TOTAL_DIGIMON_ANIMATIONS)


// Config watcher constants
#define INOTIFY_EVENT_SIZE (sizeof(struct inotify_event))
#define INOTIFY_BUF_LEN (1024 * (INOTIFY_EVENT_SIZE + 16))

// Config watcher structure
typedef struct {
    int inotify_fd;
    int watch_fd;
    char *config_path;

    void (*reload_callback)(const char *config_path);

    pthread_t watcher_thread;
    sig_atomic_t _running;
} config_watcher_t;

// Config watcher function declarations
int config_watcher_init(config_watcher_t *watcher, const char *config_path, void (*callback)(const char *));
void config_watcher_start(config_watcher_t *watcher);
void config_watcher_stop(config_watcher_t *watcher);
void config_watcher_cleanup(config_watcher_t *watcher);


// Globals (Context)
typedef struct {
    int *any_key_pressed;
    sig_atomic_t _capture_input_running;
    pid_t _input_child_pid;
} input_context_t;


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

typedef struct {
    animation_t anims[TOTAL_ANIMATIONS];
    int anim_frame_index;
    pthread_mutex_t anim_lock;

    sig_atomic_t _running;
    pthread_t _anim_thread;
} animation_context_t;

#endif // BONGOCAT_H