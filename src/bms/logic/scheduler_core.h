// Port van dashboard/scheduler_core.py (EMS ae7029d) - simulatie + greedy
// zoektocht naar laad- en blokvensters. Bewust zo letterlijk mogelijk, zodat
// het naast het Python-origineel te leggen blijft.
//
// Op de ESP32 wordt de uitkomst ALLEEN gebruikt voor weergave (Scherm D) en de
// besparingsboekhouding - er gaat nooit een commando naar de batterij.
#pragma once

#include <stdbool.h>

#define SCHED_INTERVAL_MINUTES 15
#define SCHED_MAX_N 192 // 48 uur aan kwartieren
#define SCHED_MAX_WINDOWS 16
#define SCHED_MIN_MEANINGFUL_CHARGE_KWH 0.01
#define SCHED_MIN_MEANINGFUL_COST_IMPROVEMENT_EUR 0.001

typedef struct {
    double capacity_kwh;
    double max_charge_kwh_per_interval;
    double max_discharge_kwh_per_interval;
    double min_reserve_kwh;
    double degradation_cost_per_kwh;
    double round_trip_efficiency;
    bool has_max_soc;
    double max_soc_kwh;
} sched_battery_t;

// Defaults van BatteryConfig() in Python.
sched_battery_t sched_battery_default(void);

typedef struct {
    int n;
    double soc_trajectory[SCHED_MAX_N];
    double charge_plan[SCHED_MAX_N];
    double discharge_plan[SCHED_MAX_N];
    double grid_import_kwh[SCHED_MAX_N];
    double exported_kwh[SCHED_MAX_N];
    double total_cost_eur;          // afgerond op 4 decimalen, zoals round(x, 4)
    double total_degradation_eur;
    double total_export_revenue_eur;
} sched_sim_result_t;

typedef struct {
    int start_interval;
    int length_intervals;
} sched_window_t;

typedef struct {
    int scenarios_tried;
    int n_windows;
    sched_window_t windows_chosen[SCHED_MAX_WINDOWS];
    int n_block_windows;
    sched_window_t block_windows_chosen[SCHED_MAX_N + SCHED_MAX_WINDOWS]; // + veiligheidsnet-kwartieren
    double avoided_discharge_kwh[SCHED_MAX_N];
} sched_debug_t;

void sched_simulate_plan(int n, const double * import_prices, const double * export_prices, const double * load_kwh,
                         const double * solar_kwh, const double * charge_plan, const double * discharge_plan,
                         double soc_start_kwh, const sched_battery_t * battery, sched_sim_result_t * out);

// candidate_lengths NULL -> 1..8 (15 min .. 2 uur), zoals de Python-default.
// Wordt tijdens het zoeken elke 64 scenario's aangeroepen: op de ESP32 geeft
// de daemon hier even de CPU af (een plan kan >5 s rekenen; anders grijpt de
// task-watchdog in). nullptr = niets (standaard, en in de unit-tests).
typedef void (*sched_yield_fn)(void);
void sched_set_yield_hook(sched_yield_fn fn);

void sched_find_best_plan(int n, const double * import_prices, const double * export_prices, const double * load_kwh,
                          const double * solar_kwh, double soc_start_kwh, const sched_battery_t * battery,
                          const int * candidate_lengths, int n_lengths, int max_windows, int max_block_windows,
                          sched_sim_result_t * result_out, sched_debug_t * debug_out);

// "HH:MM" voor een kwartierindex (met %24-wrap, zoals interval_to_time()).
void sched_interval_to_time(int interval_index, char * out, int out_size);
