#ifndef SCREEN_SOLAR_H
#define SCREEN_SOLAR_H

#include <lvgl.h>
#include "ipc_client.h"

void screen_solar_build(lv_obj_t * parent);
void screen_solar_update(const dashboard_state_t * state);

#endif /* SCREEN_SOLAR_H */
