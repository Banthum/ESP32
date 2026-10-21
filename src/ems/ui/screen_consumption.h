#ifndef SCREEN_CONSUMPTION_H
#define SCREEN_CONSUMPTION_H

#include <lvgl.h>
#include "ipc_client.h"

void screen_consumption_build(lv_obj_t * parent);
void screen_consumption_update(const dashboard_state_t * state);

#endif /* SCREEN_CONSUMPTION_H */
