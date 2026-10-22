#ifndef SCREEN_SAVINGS_H
#define SCREEN_SAVINGS_H

#include <lvgl.h>
#include "ipc_client.h"

void screen_savings_build(lv_obj_t * parent);
void screen_savings_update(const dashboard_state_t * state);

#endif /* SCREEN_SAVINGS_H */
