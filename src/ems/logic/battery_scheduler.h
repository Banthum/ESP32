// Port van dashboard/battery_scheduler.py (EMS ae7029d): tarieven, opbouw van
// de kwartierreeksen en plan_day(). Alleen voor weergave en boekhouding op de
// ESP32 - current_decision wordt hier nooit naar de batterij gestuurd.
#pragma once

#include <stdbool.h>
#include <time.h>

#include "../core/optional.h"
#include "scheduler_core.h"

#define HOURS_PER_PLAN 48
#define QUARTERS_PER_PLAN (HOURS_PER_PLAN * 4)

// Het eigen energiecontract (EUR/kWh incl. BTW, 2026-tarieven) - zie Python.
#define INKOOPVERGOEDING_EUR_PER_KWH 0.02
#define ENERGIEBELASTING_EUR_PER_KWH 0.11085
#define ZONNEBONUS_FRACTION 0.10

double apply_import_tariff(double epex_price);
double apply_export_tariff(double epex_price, bool zonnebonus_eligible);

typedef enum { DECISION_NONE = 0, DECISION_AUTO, DECISION_CHARGE, DECISION_BLOCK } decision_action_t;

typedef struct {
    decision_action_t action; // NONE = te weinig data (Python: current_decision None)
    double power_w;
    double remaining_s;
} plan_decision_t;

typedef struct {
    plan_decision_t current_decision;
    opt_d eigen_zon_wh[QUARTERS_PER_PLAN];
    opt_d actief_gestuurd_wh[QUARTERS_PER_PLAN];
    opt_d verwacht_verbruik_wh[QUARTERS_PER_PLAN];
    opt_d soc_forecast_pct[QUARTERS_PER_PLAN];
    opt_d geblokkeerd_wh[QUARTERS_PER_PLAN];
    // debug_info, met absolute begintijden ("morgen HH:MM" voor kwartier >= 96)
    int n_windows;
    sched_window_t windows_chosen[SCHED_MAX_WINDOWS];
    char windows_start_time[SCHED_MAX_WINDOWS][16];
    int n_block_windows;
    sched_window_t block_windows_chosen[SCHED_MAX_N + SCHED_MAX_WINDOWS];
    char block_windows_start_time[SCHED_MAX_N + SCHED_MAX_WINDOWS][16];
    int scenarios_tried;
    unsigned long compute_ms;
} plan_result_t;

// Invoer als nullable reeksen: import_price_curve per uur (EPEX incl. BTW,
// lengte n_price_hours, meestal 48), consumption per kwartier vanaf middernacht
// vandaag (kWh, lengte n_consumption), solar per uur (kW, lengte n_solar_hours).
// quarter_price: optioneel (NULL), {kwartier: EUR/kWh} via valid/value-arrays.
typedef struct {
    const opt_d * import_price_curve;
    int n_price_hours;
    const opt_d * expected_consumption_curve;
    int n_consumption;
    const opt_d * solar_forecast_curve;
    int n_solar_hours;
    const bool * quarter_price_valid;
    const double * quarter_price_value;
    int n_quarter_prices;
} plan_inputs_t;

// _build_series(): vlakke kwartierreeksen vanaf het kwartier van `now`.
typedef struct {
    int n;
    int quarters[SCHED_MAX_N];
    double import_prices[SCHED_MAX_N];
    double export_prices[SCHED_MAX_N];
    double load_kwh[SCHED_MAX_N];
    double solar_kwh[SCHED_MAX_N];
} plan_series_t;

void plan_build_series(const plan_inputs_t * in, time_t now, double horizon_hours, plan_series_t * out);

// Testhaak (vervangt Python's patch.object(scheduler_core, "find_best_plan")):
// standaard sched_find_best_plan; tests mogen hem tijdelijk omzetten.
typedef void (*find_best_plan_fn)(int n, const double * ip, const double * ep, const double * load,
                                  const double * solar, double soc0, const sched_battery_t * b,
                                  const int * candidate_lengths, int n_lengths, int max_windows,
                                  int max_block_windows, sched_sim_result_t * result_out, sched_debug_t * debug_out);
extern find_best_plan_fn g_find_best_plan;

void plan_day(const plan_inputs_t * in, double soc_pct, double capacity_kwh, double max_power_w, time_t now,
              double horizon_hours, double min_soc_pct, double max_soc_pct, double energy_loss_pct,
              plan_result_t * out);
