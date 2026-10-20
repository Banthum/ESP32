#include "savings_tracker.h"

static double dmin(double a, double b)
{
    return a < b ? a : b;
}

static double dmax(double a, double b)
{
    return a > b ? a : b;
}

charge_result_t savings_charge(shadow_state_t state, double real_charge_kwh, double capacity_kwh,
                               double solar_surplus_kwh, double export_price_eur_kwh, double import_price_eur_kwh,
                               bool active_window)
{
    if(real_charge_kwh <= 0) {
        return {state, 0.0, 0.0};
    }
    double headroom = dmax(0.0, capacity_kwh - state.shadow_soc_kwh);
    double zon_kwh = active_window ? dmin(dmin(real_charge_kwh, dmax(0.0, solar_surplus_kwh)), headroom)
                                   : dmin(real_charge_kwh, headroom);
    double actief_kwh = real_charge_kwh - zon_kwh;

    shadow_state_t next = {dmin(capacity_kwh, state.shadow_soc_kwh + zon_kwh)};
    return {next, -zon_kwh * export_price_eur_kwh, -actief_kwh * import_price_eur_kwh};
}

discharge_result_t savings_discharge(shadow_state_t state, double real_discharge_kwh, double import_price_eur_kwh)
{
    if(real_discharge_kwh <= 0) {
        return {state, 0.0, 0.0};
    }
    double zon_kwh = dmin(real_discharge_kwh, state.shadow_soc_kwh);
    double ems_kwh = real_discharge_kwh - zon_kwh;
    shadow_state_t next = {state.shadow_soc_kwh - zon_kwh};
    return {next, zon_kwh * import_price_eur_kwh, ems_kwh * import_price_eur_kwh};
}

shadow_state_t savings_block(shadow_state_t state, double blocked_kwh)
{
    if(blocked_kwh <= 0) {
        return state;
    }
    double zon_kwh = dmin(blocked_kwh, state.shadow_soc_kwh);
    if(zon_kwh <= 0) {
        return state;
    }
    return {state.shadow_soc_kwh - zon_kwh};
}
