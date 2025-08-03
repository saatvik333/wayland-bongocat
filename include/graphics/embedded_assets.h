#ifndef BONGOCAT_EMBEDDED_ASSETS_H
#define BONGOCAT_EMBEDDED_ASSETS_H

#include "context.h"
#include <stddef.h>
//#include <assert.h>

/// @TODO: add more digimons
extern const unsigned char dm20_agumon_png[];
extern const size_t dm20_agumon_png_size;
#define DM20_AGUMON_SPRITE_SHEET_COLS 9
#define DM20_AGUMON_SPRITE_SHEET_ROWS 1


// anim indexes
#define BONGOCAT_ANIM_INDEX         0
#define DM20_AGUMON_ANIM_INDEX      1

static_assert(TOTAL_DIGIMON_ANIMATIONS == 1);
static_assert(TOTAL_ANIMATIONS == 2);

#endif // EMBEDDED_ASSETS_H
