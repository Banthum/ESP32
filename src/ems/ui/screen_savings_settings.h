#ifndef SCREEN_SAVINGS_SETTINGS_H
#define SCREEN_SAVINGS_SETTINGS_H

#include <lvgl.h>

/* Besparing-instellingen, reachable via the header's gear icon while the
 * Besparing tab is active (2026-10-05, on request - "een setting knop
 * zoals andere dashboards... waarin ik de tellers kan resetten"). Only
 * one action: reset the "Batterijverdiensten"/"EMS alleen" counters to
 * zero. Unlike screen_battery_settings.c/screen_price_settings.c there is
 * no config file to write - the reset clears savings_quarterly/
 * battery_pool_state directly in emsmachine.db via a spawned Python
 * helper (savings_reset_writer.py, same posix_spawn-with-real-argv
 * pattern as solar_settings_writer.py), then restarts dashboard-daemon.
 * service so the daemon's in-memory pool_state/savings_summary_cache
 * reload from the now-empty tables instead of immediately overwriting
 * the reset on its next flush. */
void screen_savings_settings_init(lv_obj_t * dashboard_screen);
void screen_savings_settings_open(void);

#endif /* SCREEN_SAVINGS_SETTINGS_H */
