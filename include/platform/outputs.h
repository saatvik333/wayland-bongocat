#ifndef BONGOCAT_OUTPUTS_H
#define BONGOCAT_OUTPUTS_H
#include "core/bongocat.h"
struct zxdg_output_manager_v1;
extern output_ref_t outputs[MAX_OUTPUTS];
extern size_t output_count;
void outputs_add(struct wl_registry *registry, uint32_t id, uint32_t version);
void outputs_set_manager(struct zxdg_output_manager_v1 *manager);
void outputs_remove(uint32_t id);
void outputs_cleanup(void);
bool outputs_take_changed(void);
#endif
