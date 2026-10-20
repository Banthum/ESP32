#include <Arduino.h>

#include "battery_scheduler.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../core/timeutil.h"

find_best_plan_fn g_find_best_plan = sched_find_best_plan;

double apply_import_tariff(double epex_price)
{
    return epex_price + INKOOPVERGOEDING_EUR_PER_KWH + ENERGIEBELASTING_EUR_PER_KWH;
}

double apply_export_tariff(double epex_price, bool zonnebonus_eligible)
{
    double base = epex_price + INKOOPVERGOEDING_EUR_PER_KWH;
    double export_price = base;
    if(zonnebonus_eligible && base > 0) {
        export_price += ZONNEBONUS_FRACTION * base;
    }
    return export_price;
}

// _build_series(): vanaf het kwartier van `now`, stopt zodra er geen prijs meer is.
static void build_series(const plan_inputs_t * in, int current_quarter, double horizon_hours, plan_series_t * s)
{
    int max_quarters = (int) lround(horizon_hours * 4);
    if(max_quarters > SCHED_MAX_N) max_quarters = SCHED_MAX_N;
    s->n = 0;
    int q = current_quarter;
    while(s->n < max_quarters) {
        int hour = q / 4;
        double epex;
        if(in->quarter_price_valid && q < in->n_quarter_prices && in->quarter_price_valid[q]) {
            epex = in->quarter_price_value[q];
        } else if(in->import_price_curve && hour < in->n_price_hours && in->import_price_curve[hour].valid) {
            epex = in->import_price_curve[hour].v;
        } else {
            // Python: `import_price_curve[hour]` zou bij None een TypeError geven -
            // in de praktijk bevat de curve nooit None (24-of-niets). Stoppen is
            // dezelfde "geen data, niet gokken"-uitkomst.
            break;
        }
        s->import_prices[s->n] = apply_import_tariff(epex);

        double solar_share = 0.0;
        if(in->solar_forecast_curve && hour < in->n_solar_hours && in->solar_forecast_curve[hour].valid) {
            solar_share = in->solar_forecast_curve[hour].v / 4.0;
        }
        s->solar_kwh[s->n] = solar_share;
        s->export_prices[s->n] = apply_export_tariff(epex, solar_share > 0);

        double consumption = 0.0;
        if(in->expected_consumption_curve && q < in->n_consumption && in->expected_consumption_curve[q].valid) {
            consumption = in->expected_consumption_curve[q].v;
        }
        s->load_kwh[s->n] = consumption;
        s->quarters[s->n] = q;
        s->n++;
        q++;
    }
}

void plan_build_series(const plan_inputs_t * in, time_t now, double horizon_hours, plan_series_t * out)
{
    struct tm local;
    localtime_r(&now, &local);
    build_series(in, (local.tm_hour * 60 + local.tm_min) / 15, horizon_hours, out);
}

static void absolute_start_time(int current_quarter, int start_interval, char * out, int out_size)
{
    int absolute_q = current_quarter + start_interval;
    char hhmm[8];
    sched_interval_to_time(absolute_q, hhmm, sizeof(hhmm));
    snprintf(out, out_size, "%s%s", absolute_q >= 96 ? "morgen " : "", hhmm);
}

void plan_day(const plan_inputs_t * in, double soc_pct, double capacity_kwh, double max_power_w, time_t now,
              double horizon_hours, double min_soc_pct, double max_soc_pct, double energy_loss_pct,
              plan_result_t * out)
{
    unsigned long t0 = millis();
    struct tm local;
    localtime_r(&now, &local);
    int current_quarter = (local.tm_hour * 60 + local.tm_min) / 15;

    memset(out, 0, sizeof(*out));
    opt_fill_none(out->eigen_zon_wh, QUARTERS_PER_PLAN);
    opt_fill_none(out->actief_gestuurd_wh, QUARTERS_PER_PLAN);
    opt_fill_none(out->verwacht_verbruik_wh, QUARTERS_PER_PLAN);
    opt_fill_none(out->soc_forecast_pct, QUARTERS_PER_PLAN);
    opt_fill_none(out->geblokkeerd_wh, QUARTERS_PER_PLAN);
    out->current_decision.action = DECISION_NONE;

    static plan_series_t s;
    build_series(in, current_quarter, horizon_hours, &s);
    if(s.n == 0) {
        out->compute_ms = millis() - t0;
        return;
    }

    double power_limit_kwh_q = max_power_w * SCHED_INTERVAL_MINUTES / 60.0 / 1000.0;
    sched_battery_t battery = sched_battery_default();
    battery.capacity_kwh = capacity_kwh;
    battery.max_charge_kwh_per_interval = power_limit_kwh_q;
    battery.max_discharge_kwh_per_interval = power_limit_kwh_q;
    battery.min_reserve_kwh = capacity_kwh * min_soc_pct / 100;
    battery.has_max_soc = true;
    battery.max_soc_kwh = capacity_kwh * max_soc_pct / 100;
    battery.round_trip_efficiency = 1 - energy_loss_pct / 100;
    double soc_start_kwh = soc_pct / 100 * capacity_kwh;

    static sched_sim_result_t result;
    static sched_debug_t dbg;
    g_find_best_plan(s.n, s.import_prices, s.export_prices, s.load_kwh, s.solar_kwh, soc_start_kwh, &battery,
                         nullptr, 0, 4, 4, &result, &dbg);

    out->scenarios_tried = dbg.scenarios_tried;
    out->n_windows = dbg.n_windows;
    for(int w = 0; w < dbg.n_windows; w++) {
        out->windows_chosen[w] = dbg.windows_chosen[w];
        absolute_start_time(current_quarter, dbg.windows_chosen[w].start_interval, out->windows_start_time[w], 16);
    }
    out->n_block_windows = dbg.n_block_windows;
    for(int w = 0; w < dbg.n_block_windows; w++) {
        out->block_windows_chosen[w] = dbg.block_windows_chosen[w];
        absolute_start_time(current_quarter, dbg.block_windows_chosen[w].start_interval,
                            out->block_windows_start_time[w], 16);
    }

    static bool in_window[SCHED_MAX_N], in_block[SCHED_MAX_N];
    memset(in_window, 0, sizeof(in_window));
    memset(in_block, 0, sizeof(in_block));
    for(int w = 0; w < dbg.n_windows; w++) {
        for(int i = dbg.windows_chosen[w].start_interval;
            i < dbg.windows_chosen[w].start_interval + dbg.windows_chosen[w].length_intervals; i++) {
            in_window[i] = true;
        }
    }
    for(int w = 0; w < dbg.n_block_windows; w++) {
        int end = dbg.block_windows_chosen[w].start_interval + dbg.block_windows_chosen[w].length_intervals;
        if(end > s.n) end = s.n;
        for(int i = dbg.block_windows_chosen[w].start_interval; i < end; i++) in_block[i] = true;
    }

    for(int i = 0; i < s.n; i++) {
        int q = s.quarters[i];
        if(q >= QUARTERS_PER_PLAN) break;
        double charge_wh = result.charge_plan[i] * 1000.0;
        if(in_window[i]) {
            out->actief_gestuurd_wh[q] = opt_some(charge_wh);
            out->eigen_zon_wh[q] = opt_some(0.0);
        } else {
            out->eigen_zon_wh[q] = opt_some(charge_wh);
            out->actief_gestuurd_wh[q] = opt_some(0.0);
        }
        out->verwacht_verbruik_wh[q] = opt_some(s.load_kwh[i] * 1000.0);
        out->soc_forecast_pct[q] = opt_some(result.soc_trajectory[i] / capacity_kwh * 100.0);
        if(in_block[i]) {
            double avoided_wh = dbg.avoided_discharge_kwh[i] * 1000.0;
            double verbruik = out->verwacht_verbruik_wh[q].v;
            out->geblokkeerd_wh[q] = opt_some(avoided_wh > verbruik ? avoided_wh : verbruik);
        } else {
            out->geblokkeerd_wh[q] = opt_some(0.0);
        }
    }

    // Venster dat "nu" dekt: alleen via start_interval == 0. Laden gaat vóór blokkeren.
    out->current_decision.action = DECISION_AUTO;
    ems_date_t today = date_from_epoch_local(now);
    for(int w = 0; w < dbg.n_windows; w++) {
        if(dbg.windows_chosen[w].start_interval == 0) {
            int end_q = current_quarter + dbg.windows_chosen[w].length_intervals;
            time_t end = local_midnight(today) + (time_t) end_q * SCHED_INTERVAL_MINUTES * 60;
            double remaining = difftime(end, now);
            out->current_decision = {DECISION_CHARGE, -max_power_w, remaining > 0 ? remaining : 0.0};
            out->compute_ms = millis() - t0;
            return;
        }
    }
    for(int w = 0; w < dbg.n_block_windows; w++) {
        if(dbg.block_windows_chosen[w].start_interval == 0) {
            int end_q = current_quarter + dbg.block_windows_chosen[w].length_intervals;
            time_t end = local_midnight(today) + (time_t) end_q * SCHED_INTERVAL_MINUTES * 60;
            double remaining = difftime(end, now);
            out->current_decision = {DECISION_BLOCK, 0.0, remaining > 0 ? remaining : 0.0};
            break;
        }
    }
    out->compute_ms = millis() - t0;
}
