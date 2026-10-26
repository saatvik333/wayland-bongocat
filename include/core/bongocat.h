#ifndef BONGOCAT_H
#define BONGOCAT_H

#define BONGOCAT_VERSION "2.1.1"

#define MAX_OUTPUTS 8  // Monitors tracked at once

// Cat artwork: frames and the cropped SVG aspect ratio.
#define NUM_FRAMES       5
#define CAT_IMAGE_WIDTH  500
#define CAT_IMAGE_HEIGHT 277

#define BONGOCAT_FRAME_BOTH_UP    0
#define BONGOCAT_FRAME_LEFT_DOWN  1
#define BONGOCAT_FRAME_RIGHT_DOWN 2
#define BONGOCAT_FRAME_BOTH_DOWN  3
#define BONGOCAT_FRAME_SLEEPING   4

#endif  // BONGOCAT_H
