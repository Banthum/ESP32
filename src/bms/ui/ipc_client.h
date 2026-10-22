#ifndef IPC_CLIENT_H
#define IPC_CLIENT_H

#include <stdbool.h>

#define IPC_HISTORY_MAX 60   /* must match dashboard/data_sources.py HISTORY_LENGTH */
#define IPC_PRICE_CURVE_LEN 24
#define IPC_STRING_MAX 32
#define IPC_QUARTERS_PER_DAY 96   /* must match dashboard/storage.py QUARTERS_PER_DAY */
#define IPC_SOLAR_FORECAST_LEN 24
/* 192 kwartieren = 48h (vandaag+morgen) - must match
 * dashboard/battery_scheduler.py's QUARTERS_PER_PLAN (2026-10-02: switched
 * from 48 hourly buckets to 192 quarterly ones, Jeroen: "ik wil dat elke
 * grafiek dezelfde resolutie heeft als de data waarop de batterijlader
 * werkt" - scheduler_core.py already simulates per-quarter internally).
 * Before that (2026-09-30): extended from 24h to match a 2-day forecast
 * horizon. */
#define IPC_BATTERY_PLAN_LEN 192

/* Plain-C snapshot of the latest dashboard message. No LVGL types here on
 * purpose: this struct is written by the socket-reader background thread
 * and read by the LVGL-thread lv_timer in dashboard_ui.c (see
 * docs/ipc_protocol.md's "Faalgedrag" section) — LVGL itself is not
 * thread-safe, so nothing in this file may touch an lv_obj_t. */
typedef struct {
    bool connected; /* true once the socket is up AND at least one message parsed since */

    bool battery_power_w_valid;
    double battery_power_w;

    bool battery_state_valid;
    char battery_state[IPC_STRING_MAX];

    bool battery_soc_pct_valid;
    int battery_soc_pct;

    bool solar_power_w_valid;
    double solar_power_w;

    bool grid_power_w_valid;
    double grid_power_w;

    bool price_now_valid;
    double price_now;

    bool price_curve_valid;
    double price_curve[IPC_PRICE_CURVE_LEN];

    /* Terugleverprijs (2026-10-01) - same all-or-nothing shape as
     * price_curve_valid above, see docs/ipc_protocol.md's
     * export_price_curve entry. Zonder salderingsrekening, see
     * battery_scheduler.apply_export_tariff()'s docstring. */
    bool export_price_curve_valid;
    double export_price_curve[IPC_PRICE_CURVE_LEN];

    /* Quarter-resolution, 2-daags (192-point, vandaag+morgen, sinds
     * 2026-10-08 - was 96/today-only) versies van price_curve/
     * export_price_curve hierboven - wat de Kosten-chart nu plot, zie
     * docs/ipc_protocol.md's price_curve_quarterly entry. Per-element
     * nullable (niet één blanket _valid-vlag), zelfde stijl als
     * solar_forecast_curve_quarterly/expected_consumption_curve_quarterly
     * hieronder - morgen's prijs kan een paar uur ontbreken totdat
     * EnergyZero hem publiceert. */
    bool price_curve_quarterly_valid[IPC_BATTERY_PLAN_LEN];
    double price_curve_quarterly[IPC_BATTERY_PLAN_LEN];
    bool export_price_curve_quarterly_valid[IPC_BATTERY_PLAN_LEN];
    double export_price_curve_quarterly[IPC_BATTERY_PLAN_LEN];

    /* Gas price - added 2026-09-27, toggleable/unit-switchable via
     * screen_price_settings.c. gas_price_curve_valid is one flag for the
     * whole array (unlike expected_consumption_curve etc. below), same
     * all-or-nothing shape as price_curve_valid above - the Python side
     * sends the whole field `null` rather than per-element, see
     * docs/ipc_protocol.md. false whenever gas_enabled is false, so
     * screen_price.c can check just this one flag to decide whether to
     * draw/show anything gas-related at all. */
    bool gas_enabled;
    bool gas_price_curve_valid;
    double gas_price_curve[IPC_PRICE_CURVE_LEN];
    char gas_price_unit[IPC_STRING_MAX]; /* "EUR/kWh" or "EUR/m3" */

    int power_history_len; /* <= IPC_HISTORY_MAX, same length for all 3 series */
    double power_history_solar[IPC_HISTORY_MAX];
    double power_history_battery[IPC_HISTORY_MAX];
    double power_history_grid[IPC_HISTORY_MAX];

    /* Per-element nullable (unlike the *_valid flags above, which are one
     * flag for a whole field): expected_consumption_curve/solar_forecast_
     * curve can have individual missing quarters/hours while the rest of
     * the array is real data (see docs/ipc_protocol.md) — each slot needs
     * its own validity bit, not one blanket flag for the whole array. */
    bool expected_consumption_curve_valid[IPC_QUARTERS_PER_DAY];
    double expected_consumption_curve[IPC_QUARTERS_PER_DAY];

    /* "Werkelijk verbruik" counterpart to the above - same shape/nullability,
     * see docs/ipc_protocol.md. */
    bool actual_consumption_curve_valid[IPC_QUARTERS_PER_DAY];
    double actual_consumption_curve[IPC_QUARTERS_PER_DAY];

    bool solar_forecast_curve_valid[IPC_SOLAR_FORECAST_LEN];
    double solar_forecast_curve[IPC_SOLAR_FORECAST_LEN];

    /* Kwartier-resolutie, 2-daags (192-point, vandaag+morgen, since
     * 2026-10-04 - was 96/today-only) linearly-interpolated version of
     * solar_forecast_curve above, see docs/ipc_protocol.md's
     * solar_forecast_curve_quarterly entry - what screen_solar.c actually
     * plots now. Reuses IPC_BATTERY_PLAN_LEN (same 192 = vandaag+morgen
     * meaning as the battery plan arrays below, not actually
     * battery-specific) rather than inventing a second same-valued
     * constant. */
    bool solar_forecast_curve_quarterly_valid[IPC_BATTERY_PLAN_LEN];
    double solar_forecast_curve_quarterly[IPC_BATTERY_PLAN_LEN];

    /* Verwacht verbruik per kwartier, 2-daags (192-point, vandaag+morgen,
     * 2026-10-04) - nieuw veld voor Scherm C's staafdiagram (Jeroen: "ik
     * wil dat het verbruik per kwartier in staafdiagrammen daaraan
     * toegevoegd wordt"). Bewust een EIGEN veld, niet hergebruikt van
     * battery_plan_verwacht_verbruik_wh (die is ook al 192-punten, maar
     * alleen gevuld "vanaf nu" en zou dit scherm onnodig laten meefalen
     * als de batterijplanner ooit geen plan kan maken - zie
     * docs/ipc_protocol.md). */
    bool expected_consumption_curve_quarterly_valid[IPC_BATTERY_PLAN_LEN];
    double expected_consumption_curve_quarterly[IPC_BATTERY_PLAN_LEN];

    /* Customer's installed kWp from onboarding - used to scale Scherm C's
     * Y-axis (see screen_solar.c). Not an array like the curve above:
     * one fixed value, not per-hour. */
    bool solar_kwp_valid;
    double solar_kwp;

    /* Battery capacity from onboarding or the Batterij-instellingen
     * screen - used to scale Scherm D's Y-axis (see screen_battery.c).
     * Always valid (the Python side never omits it - a default fills in
     * server-side if unconfigured, see docs/ipc_protocol.md) but still
     * carries its own _valid flag for consistency with every other field
     * here and to survive a malformed/partial message the same
     * defensive way. */
    bool battery_capacity_kwh_valid;
    double battery_capacity_kwh;

    /* Dynamic Energiestromen "Home" floor (2026-10-01) - replaces
     * screen_energyflow.c's old hardcoded MIN_HOME_W constant, see
     * docs/ipc_protocol.md's min_home_w entry. Always valid, same
     * "server-side default fills in" reasoning as battery_capacity_kwh
     * above. */
    bool min_home_w_valid;
    double min_home_w;

    /* BMSmachine's own price/zon-gestuurde laadplanning (2026-09-30,
     * replaced screen_battery.c's synthetic test data; switched from
     * per-hour to per-kwartier 2026-10-02, see IPC_BATTERY_PLAN_LEN
     * above) - same per-element nullability as expected_consumption_curve/
     * solar_forecast_curve above (a kwartier before "now" has no plan yet,
     * see dashboard/battery_scheduler.py::plan_day()'s own None
     * convention). */
    bool battery_plan_eigen_zon_wh_valid[IPC_BATTERY_PLAN_LEN];
    double battery_plan_eigen_zon_wh[IPC_BATTERY_PLAN_LEN];
    bool battery_plan_actief_gestuurd_wh_valid[IPC_BATTERY_PLAN_LEN];
    double battery_plan_actief_gestuurd_wh[IPC_BATTERY_PLAN_LEN];
    bool battery_plan_verwacht_verbruik_wh_valid[IPC_BATTERY_PLAN_LEN];
    double battery_plan_verwacht_verbruik_wh[IPC_BATTERY_PLAN_LEN];
    /* Hoeveel zelfconsumptie-ontladen in dat kwartier daadwerkelijk
     * voorkomen is door een actief "block"-venster (2026-10-15, zie
     * scheduler_core.find_best_plan()'s block_windows_chosen/
     * avoided_discharge_kwh) - puur weergave, zelfde nullability-conventie
     * als de 3 velden hierboven. */
    bool battery_plan_geblokkeerd_wh_valid[IPC_BATTERY_PLAN_LEN];
    double battery_plan_geblokkeerd_wh[IPC_BATTERY_PLAN_LEN];
    bool battery_soc_forecast_pct_valid[IPC_BATTERY_PLAN_LEN];
    double battery_soc_forecast_pct[IPC_BATTERY_PLAN_LEN];

    /* "Actieve aansturing" switch state (screen_battery_settings.c) - no
     * separate _valid flag, same "defaults false if the field is somehow
     * missing" style as gas_enabled above (the Python side always sends
     * this, see docs/ipc_protocol.md). */
    bool battery_active_control_enabled;

    /* "Batterijverdiensten"/"EMS alleen" savings counters (2026-10-03),
     * see dashboard/savings_tracker.py's module docstring for the full
     * accounting design and docs/ipc_protocol.md for these 10 fields.
     * Always valid (the Python side uses COALESCE, so "nothing saved
     * yet" is a real 0.0, never missing) - same "always valid" reasoning
     * as battery_capacity_kwh/min_home_w above, one shared _valid flag
     * for all 10 rather than 10 separate ones since they're always sent
     * or never sent together as a group. */
    bool savings_valid;
    double savings_dag_batterij_eur;
    double savings_dag_ems_eur;
    double savings_week_batterij_eur;
    double savings_week_ems_eur;
    double savings_maand_batterij_eur;
    double savings_maand_ems_eur;
    double savings_jaar_batterij_eur;
    double savings_jaar_ems_eur;
    double savings_altijd_batterij_eur;
    double savings_altijd_ems_eur;
} dashboard_state_t;

/* ESP32: alloceert de toestand in PSRAM. Aanroepen vóór dashboard_ui_init(). */
void ipc_client_init(void);

/* Start de taak die nieuwe berichten van de state bus (de daemon) oppakt. */
void ipc_client_start(void);

/* Stabiele kopie van de laatst bekende toestand. Alleen aanroepen vanuit de
 * LVGL-taak; de pointer blijft geldig tot de volgende aanroep. (Op de Pi:
 * kopie naar een struct op de stack - hier ~25 KB, dus een vaste PSRAM-buffer.) */
const dashboard_state_t * ipc_client_get_state(void);

#endif /* IPC_CLIENT_H */
