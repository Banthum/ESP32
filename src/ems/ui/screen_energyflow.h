#ifndef SCREEN_ENERGYFLOW_H
#define SCREEN_ENERGYFLOW_H

#include <lvgl.h>
#include "ipc_client.h"

/* "Energiestromen" tab (added 2026-09-27, on request) - a simplified
 * from-scratch approximation of Home Assistant's energy-flow Sankey card:
 * 4 circular nodes (Net/Zon/Home/Batterij) connected by curved lines with
 * an animated dot showing live flow direction. LVGL has no built-in
 * bezier-curve or flow-diagram widget, so the curves are pre-computed
 * point arrays fed to lv_line, and each dot is a small circle animated
 * along that same point array via a custom lv_anim exec callback - see
 * screen_energyflow.c for the full approach and its limitations vs. the
 * real HA card. All data already exists in dashboard_state_t (battery/
 * solar/grid) - no new IPC field needed. `Home`'s own power isn't a
 * separate field anywhere in this app; this screen derives it locally
 * (solar + grid + battery, using this app's existing sign conventions)
 * purely for its own display - this is NOT the same as solving
 * storage.py's documented "real household load" gap system-wide. */
void screen_energyflow_build(lv_obj_t * parent);
void screen_energyflow_update(const dashboard_state_t * state);

#endif /* SCREEN_ENERGYFLOW_H */
