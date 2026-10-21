#ifndef SCREEN_BATTERY_H
#define SCREEN_BATTERY_H

#include <lvgl.h>
#include "ipc_client.h"

/* EMSmachine's own price/zon-gestuurde laadplanning (2026-09-30), driven
 * by dashboard_state_t's battery_plan_eigen_zon_wh/
 * battery_plan_actief_gestuurd_wh/battery_plan_verwacht_verbruik_wh/
 * battery_soc_forecast_pct - replaced the earlier synthetic placeholder
 * data (see CLAUDE.md git history if that era is ever relevant again).
 * screen_battery_update() also watches battery_capacity_kwh and rescales
 * both the Y-axis and the batterijlading series to it whenever it
 * changes, same as before. */
void screen_battery_build(lv_obj_t * parent);
void screen_battery_update(const dashboard_state_t * state);

#endif /* SCREEN_BATTERY_H */
