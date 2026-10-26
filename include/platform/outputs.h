#ifndef BONGOCAT_OUTPUTS_H
#define BONGOCAT_OUTPUTS_H

#include "core/bongocat.h"

#include <stdbool.h>
#include <stdint.h>

struct wl_output;
struct wl_registry;
struct zxdg_output_v1;
struct zxdg_output_manager_v1;

typedef struct {
  struct wl_output *wl_output;  // NULL for a free slot
  struct zxdg_output_v1 *xdg_output;
  uint32_t id;     // Registry name
  char name[128];  // wl_output or xdg-output name
  int width;       // Logical size; 0 until known
  int height;
  int scale;  // Integer wl_output scale
  int mode_width;
  int mode_height;
  int transform;
  int xdg_width;
  int xdg_height;
} output_ref_t;

extern output_ref_t outputs[MAX_OUTPUTS];

void outputs_add(struct wl_registry *registry, uint32_t id, uint32_t version);
void outputs_set_manager(struct zxdg_output_manager_v1 *manager);
void outputs_remove(uint32_t id);
void outputs_cleanup(void);
bool outputs_take_changed(void);

#endif  // BONGOCAT_OUTPUTS_H
