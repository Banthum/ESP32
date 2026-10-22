#ifndef SCREEN_PRICE_H
#define SCREEN_PRICE_H

#include <lvgl.h>
#include "ipc_client.h"

void screen_price_build(lv_obj_t * parent);
void screen_price_update(const dashboard_state_t * state);

#endif /* SCREEN_PRICE_H */
