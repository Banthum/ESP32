// Port van dashboard/savings_tracker.py (versie ae7029d, incl. block()):
// shadow-SOC-boekhouding voor "Batterijverdiensten" vs "EMS alleen".
// Pure functies zonder I/O; daemon.cpp bewaart de state via storage.
#pragma once

#include <stdbool.h>

#define SAVINGS_IDLE_DEADBAND_W 20

typedef struct {
    double shadow_soc_kwh;
} shadow_state_t;

typedef struct {
    shadow_state_t state;
    double batterij_cash_flow_eur; // <= 0, zon-aandeel (gemiste exportopbrengst)
    double ems_cash_flow_eur;      // <= 0, actief-aandeel (betaalde importkosten)
} charge_result_t;

typedef struct {
    shadow_state_t state;
    double batterij_savings_eur;
    double ems_savings_eur;
} discharge_result_t;

charge_result_t savings_charge(shadow_state_t state, double real_charge_kwh, double capacity_kwh,
                               double solar_surplus_kwh, double export_price_eur_kwh, double import_price_eur_kwh,
                               bool active_window);

discharge_result_t savings_discharge(shadow_state_t state, double real_discharge_kwh, double import_price_eur_kwh);

// Blokvenster: verlaagt alleen de shadow-SOC, geen cashflow (zie Python-docstring).
shadow_state_t savings_block(shadow_state_t state, double blocked_kwh);
