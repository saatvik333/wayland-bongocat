#ifndef EMBEDDED_ASSETS_H
#define EMBEDDED_ASSETS_H

#include "embedded_assets_bongocat.h"
#include <stddef.h>

/// @TODO: move digimon animations where needed, in animation.c
extern const unsigned char dm20_agumon_png[];
extern const size_t dm20_agumon_png_size;
#define DM20_AGUMON_SPRITE_SHEET_COLS 9
#define DM20_AGUMON_SPRITE_SHEET_ROWS 1


#define BONGOCAT_ANIM_INDEX  0

#define DM20_AGUMON_ANIM_INDEX    1

#endif // EMBEDDED_ASSETS_H
