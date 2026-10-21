#ifndef DASHBOARD_UI_H
#define DASHBOARD_UI_H

/* Builds all widgets on the active screen and registers the lv_timer that
 * applies ipc_client's shared state into them. Call once after lv_init()
 * and the display driver are set up. */
void dashboard_ui_init(void);

/* ESP32-diagnose (/api/debug/tab): toont tab `index` (0-5, volgorde van
 * TAB_NAMES) en houdt de hamburger-selectie in sync. Aanroepen met de
 * LVGL-lock vast. */
void dashboard_ui_select_tab(int index);

#endif /* DASHBOARD_UI_H */
