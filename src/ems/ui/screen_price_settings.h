#ifndef SCREEN_PRICE_SETTINGS_H
#define SCREEN_PRICE_SETTINGS_H

#include <lvgl.h>

/* Gas-price display settings, reachable via the header's gear icon while
 * the Kosten tab is active - on/off switch plus a EUR/kWh<->EUR/m3 unit
 * dropdown for the gas line screen_price.c added 2026-09-27. Same simple
 * "write /etc/emsmachine/*.json directly via cJSON, no external API call
 * needed" style as screen_battery_settings.c. */
void screen_price_settings_init(lv_obj_t * dashboard_screen);
void screen_price_settings_open(void);

#endif /* SCREEN_PRICE_SETTINGS_H */
