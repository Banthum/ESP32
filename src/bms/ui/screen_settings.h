#ifndef SCREEN_SETTINGS_H
#define SCREEN_SETTINGS_H

#include <lvgl.h>

/* Creates the (initially not loaded) settings screen. `dashboard_screen`
 * is the main dashboard screen object to return to via its own "Terug"
 * button - call once at startup, after the dashboard screen itself
 * exists. */
void screen_settings_init(lv_obj_t * dashboard_screen);

/* Switches to the settings screen (lv_screen_load()) and refreshes its
 * fields from /etc/emsmachine/solar_config.json. */
void screen_settings_open(void);

#endif /* SCREEN_SETTINGS_H */
