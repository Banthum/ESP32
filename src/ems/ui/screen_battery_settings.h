#ifndef SCREEN_BATTERY_SETTINGS_H
#define SCREEN_BATTERY_SETTINGS_H

#include <lvgl.h>

/* Batterij-capaciteit setting, reachable via the header's gear icon while
 * the Batterij tab is active - mirror of screen_settings.c's Zon-specific
 * settings screen, but far simpler (one numeric field, no external API
 * call needed to save it, so no posix_spawn'd Python helper here - this
 * writes /etc/emsmachine/battery_config.json directly via cJSON). */
void screen_battery_settings_init(lv_obj_t * dashboard_screen);
void screen_battery_settings_open(void);

#endif /* SCREEN_BATTERY_SETTINGS_H */
