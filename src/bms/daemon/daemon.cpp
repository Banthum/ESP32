#include <Arduino.h>

#include "daemon.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_random.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include "../clients/marstek_client.h"
#include "../clients/p1_client.h"
#include "../clients/price_client.h"
#include "../clients/solar_forecast_client.h"
#include "../config/config.h"
#include "../core/log.h"
#include "../core/optional.h"
#include "../core/paths.h"
#include "../core/state_bus.h"
#include "../core/timeutil.h"
#include "../logic/battery_control.h"
#include "../logic/battery_scheduler.h"
#include "../logic/scheduler_core.h"
#include "../logic/savings_tracker.h"
#include "../net/net.h"
#include "../storage/storage.h"

static const char * TAG = "dashboard.daemon";

// --- constanten uit dashboard_daemon.py (zelfde waarden tenzij vermeld) ---
// Pi: 10 s. In schaduw-modus 30 s (de Pi pollt de batterij al); zodra de ESP32
// zelf aanstuurt 10 s, net als de Pi - de verificatie elke cyclus heeft dat nodig.
static const uint32_t PUSH_INTERVAL_S = BMS_BATTERY_CONTROL_LIVE ? 10 : 30;
static const double JITTER_CAP_S = 2.5;
static const uint32_t FLUSH_INTERVAL_S = 60;
static const double MAX_SAVINGS_GAP_S = 60;
static const uint32_t REPLAN_INTERVAL_S = 5 * 60;
static const double BATTERY_POWER_DEDUP_THRESHOLD_W = 25; // ook de "~0 W = blok"-tolerantie
static const uint32_t SOLAR_FORECAST_POLL_INTERVAL_S = 3 * 3600;
static const uint32_t PRICE_POLL_INTERVAL_S = 3600;
static const uint32_t PRICE_RETRY_INTERVAL_S = 5 * 60;
static const char * CONSUMPTION_SOURCE = "household_load";
static const double MAX_BASELINE_LOAD_W = 150.0;
// data_sources.HISTORY_LENGTH - 60 samples (op de ESP32 dus 30 min i.p.v. 10 min)
#define HISTORY_LENGTH 60
#define Q2 QUARTERS_PER_PLAN // 192 = vandaag + morgen

// Wat de Pi op dit moment met de batterij doet, alleen-lezend afgeleid
// (vervangt last_sent_action van de Pi voor de besparingsboekhouding).
typedef enum { PI_ACTION_NONE = 0, PI_ACTION_CHARGE, PI_ACTION_BLOCK } pi_action_t;

typedef struct {
    // in-memory ringbuffer (data_sources.HistoryBuffer)
    int hist_len;
    opt_d hist_solar[HISTORY_LENGTH], hist_battery[HISTORY_LENGTH], hist_grid[HISTORY_LENGTH];

    // dagelijks herberekend
    bool have_day_curves;
    ems_date_t last_aggregated_date;
    double min_home_w;
    opt_d expected_today[QUARTERS_PER_DAY], expected_tomorrow[QUARTERS_PER_DAY];
    bool expected_quarterly_has_tomorrow;
    opt_d actual_today[QUARTERS_PER_DAY];

    // zon
    bool solar_curve_known; // false = None (geen config/nooit opgehaald)
    opt_d solar_today[24], solar_tomorrow[24];
    opt_d solar_q_today[QUARTERS_PER_DAY], solar_q_tomorrow[QUARTERS_PER_DAY];
    bool no_solar_panels;
    bool solar_kwp_known;
    double solar_kwp;
    uint32_t last_solar_fetch_s;

    // prijzen
    bool price_known;
    double price_today[24];
    bool price_tomorrow_known;
    double price_tomorrow[24];
    uint32_t last_price_fetch_s;
    uint32_t price_poll_interval_s;
    bool quarter_known;
    quarter_price_curve_t quarter;
    gas_config_t gas;
    bool gas_known;
    double gas_curve[24];

    // batterij-instellingen + plan
    battery_config_t battery;
    uint32_t last_replan_s;
    bool replanned_once;
    plan_result_t plan;

    // besparingen
    shadow_state_t pool;
    savings_summary_t savings_cache;
    bool have_last_savings_tick;
    uint32_t last_savings_tick_ms;
    uint32_t last_flush_s;

    pi_action_t pi_action;

    // batterij-aansturing (battery_control.h). raw_decision = de beslissing
    // van de laatste replan die er een had (Python: raw_decision).
    bool has_raw_decision;
    plan_decision_t raw_decision;
    bool ctrl_initialized;
    ctrl_state_t ctrl;
    uint32_t ctrl_commands; // commando's (schaduw: die verstuurd zouden zijn)
    bool have_last_cmd;
    ctrl_command_t last_cmd;
    time_t last_cmd_at;

    // testvenster (daemon_control_test)
    bool test_active;
    plan_decision_t test_decision;
    double test_until_s;
} daemon_state_t;

static daemon_state_t * S = nullptr;
static volatile bool s_reload_requested = false;
static char s_marstek_host[40];

// Testverzoek van de HTTP-taak, opgepakt in de volgende cyclus.
static portMUX_TYPE s_test_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_test_pending = false;
static plan_decision_t s_test_request;
static double s_test_request_seconds;

void daemon_reload_config(void)
{
    s_reload_requested = true;
}

static uint32_t now_s(void)
{
    return millis() / 1000;
}

// ---------------------------------------------------------------- JSON-hulp

static cJSON * opt_array(const opt_d * v, int n)
{
    cJSON * arr = cJSON_CreateArray();
    for(int i = 0; i < n; i++) {
        cJSON_AddItemToArray(arr, v[i].valid ? cJSON_CreateNumber(v[i].v) : cJSON_CreateNull());
    }
    return arr;
}

static void add_opt(cJSON * obj, const char * key, bool valid, double v)
{
    if(valid) cJSON_AddNumberToObject(obj, key, v);
    else cJSON_AddNullToObject(obj, key);
}

static void add_opt_array(cJSON * obj, const char * key, bool known, const opt_d * v, int n)
{
    if(known) cJSON_AddItemToObject(obj, key, opt_array(v, n));
    else cJSON_AddNullToObject(obj, key);
}

static void add_double_array(cJSON * obj, const char * key, bool known, const double * v, int n)
{
    if(known) cJSON_AddItemToObject(obj, key, cJSON_CreateDoubleArray(v, n));
    else cJSON_AddNullToObject(obj, key);
}

// ---------------------------------------------------------------- config

static void read_configs(void)
{
    S->battery = config_read_battery(BMS_BATTERY_CONFIG_PATH);
    S->gas = config_read_gas(BMS_GAS_CONFIG_PATH);
    config_read_marstek_host(BMS_MARSTEK_CONFIG_PATH, s_marstek_host, sizeof(s_marstek_host));

    cJSON * solar = config_read_solar(BMS_SOLAR_CONFIG_PATH);
    S->no_solar_panels = solar && cJSON_IsTrue(cJSON_GetObjectItem(solar, "no_solar_panels"));
    const cJSON * kwp = solar ? cJSON_GetObjectItem(solar, "kwp") : nullptr;
    S->solar_kwp_known = cJSON_IsNumber(kwp);
    S->solar_kwp = S->solar_kwp_known ? kwp->valuedouble : 0.0;
    cJSON_Delete(solar);
}

// Pi: message["settings"] = {solar, battery, gas} - ruwe inhoud, elke cyclus vers.
static cJSON * settings_json(void)
{
    cJSON * s = cJSON_CreateObject();
    cJSON * solar = config_read_solar(BMS_SOLAR_CONFIG_PATH);
    if(solar) cJSON_AddItemToObject(s, "solar", solar);
    else cJSON_AddNullToObject(s, "solar");

    battery_config_t b = config_read_battery(BMS_BATTERY_CONFIG_PATH);
    cJSON * bj = cJSON_AddObjectToObject(s, "battery");
    cJSON_AddNumberToObject(bj, "capacity_kwh", b.capacity_kwh);
    cJSON_AddNumberToObject(bj, "max_power_w", b.max_power_w);
    cJSON_AddBoolToObject(bj, "active_control_enabled", b.active_control_enabled);
    cJSON_AddNumberToObject(bj, "min_soc_pct", b.min_soc_pct);
    cJSON_AddNumberToObject(bj, "max_soc_pct", b.max_soc_pct);
    cJSON_AddNumberToObject(bj, "energy_loss_pct", b.energy_loss_pct);

    gas_config_t g = config_read_gas(BMS_GAS_CONFIG_PATH);
    cJSON * gj = cJSON_AddObjectToObject(s, "gas");
    cJSON_AddBoolToObject(gj, "enabled", g.enabled);
    cJSON_AddStringToObject(gj, "unit", g.unit_is_m3 ? "m3" : "kwh");
    return s;
}

// ---------------------------------------------------------------- zon

static void zero_solar_curves(void)
{
    for(int h = 0; h < 24; h++) S->solar_today[h] = S->solar_tomorrow[h] = opt_some(0.0);
    for(int q = 0; q < QUARTERS_PER_DAY; q++) S->solar_q_today[q] = S->solar_q_tomorrow[q] = opt_some(0.0);
    S->solar_curve_known = true;
}

static void refresh_solar_curves_from_storage(void)
{
    ems_date_t today = date_today();
    ems_date_t tomorrow = date_add_days(today, 1);
    storage_solar_forecast_curve(today, S->solar_today);
    storage_solar_forecast_curve(tomorrow, S->solar_tomorrow);
    storage_solar_forecast_curve_quarterly(today, S->solar_q_today);
    storage_solar_forecast_curve_quarterly(tomorrow, S->solar_q_tomorrow);
    S->solar_curve_known = true;
    if(S->no_solar_panels) zero_solar_curves();
}

static void poll_solar(void)
{
    S->last_solar_fetch_s = now_s();
    cJSON * config = config_read_solar(BMS_SOLAR_CONFIG_PATH);
    S->no_solar_panels = config && cJSON_IsTrue(cJSON_GetObjectItem(config, "no_solar_panels"));
    if(!config) {
        BMS_LOGI(TAG, "No solar_config.json yet (onboarding not done / solar step skipped)");
        return;
    }
    if(S->no_solar_panels) {
        S->solar_kwp_known = false;
        zero_solar_curves();
        cJSON_Delete(config);
        return;
    }
    double lat = cJSON_GetNumberValue(cJSON_GetObjectItem(config, "lat"));
    double lon = cJSON_GetNumberValue(cJSON_GetObjectItem(config, "lon"));
    double dec = cJSON_GetNumberValue(cJSON_GetObjectItem(config, "declination"));
    double az = cJSON_GetNumberValue(cJSON_GetObjectItem(config, "azimuth"));
    double kwp = cJSON_GetNumberValue(cJSON_GetObjectItem(config, "kwp"));
    const cJSON * f = cJSON_GetObjectItem(config, "forecast_factor");
    double factor = cJSON_IsNumber(f) ? f->valuedouble : 1.0;
    S->solar_kwp_known = true;
    S->solar_kwp = kwp;
    cJSON_Delete(config);

    static solar_row_t rows[SOLAR_MAX_ROWS];
    static storage_solar_row_t srows[SOLAR_MAX_ROWS];
    int n = 0;
    char reason[96];
    if(!solar_fetch_forecast(lat, lon, dec, az, kwp, rows, SOLAR_MAX_ROWS, &n, reason, sizeof(reason))) {
        BMS_LOGW(TAG, "Forecast.Solar fetch failed: %s", reason);
        return;
    }
    for(int i = 0; i < n; i++) {
        memcpy(srows[i].date, rows[i].date, sizeof(srows[i].date));
        srows[i].hour = rows[i].hour;
        srows[i].watts = rows[i].watts * factor;
    }
    storage_write_solar_forecast(srows, n);
    refresh_solar_curves_from_storage();
}

// ---------------------------------------------------------------- prijzen

static void poll_prices(void)
{
    S->last_price_fetch_s = now_s();
    ems_date_t today = date_today();
    double curve[24];
    if(price_fetch_price_curve(today, curve)) {
        memcpy(S->price_today, curve, sizeof(curve));
        S->price_known = true;
        S->price_poll_interval_s = PRICE_POLL_INTERVAL_S;
    } else {
        BMS_LOGW(TAG, "EnergyZero price fetch failed, keeping previous curve if any - retrying in %us",
                 (unsigned) PRICE_RETRY_INTERVAL_S);
        S->price_poll_interval_s = PRICE_RETRY_INTERVAL_S;
    }

    if(price_fetch_price_curve(date_add_days(today, 1), curve)) {
        memcpy(S->price_tomorrow, curve, sizeof(curve));
        S->price_tomorrow_known = true;
    } else if(S->price_known) {
        BMS_LOGI(TAG, "Tomorrow's price not published yet, using today's curve as a stand-in");
        memcpy(S->price_tomorrow, S->price_today, sizeof(curve));
        S->price_tomorrow_known = true;
    }

    if(S->gas.enabled) {
        double gas_m3[24];
        if(price_fetch_gas_price_curve(today, gas_m3)) {
            for(int h = 0; h < 24; h++) S->gas_curve[h] = S->gas.unit_is_m3 ? gas_m3[h] : gas_m3[h] / GAS_KWH_PER_M3;
            S->gas_known = true;
        } else {
            BMS_LOGW(TAG, "EnergyZero gas price fetch failed, keeping previous curve if any");
        }
    }
}

// ---------------------------------------------------------------- dagwissel

static void daily_refresh(ems_date_t today)
{
    ems_date_t yesterday_d = date_add_days(today, -1);
    char yesterday[11];
    date_iso(yesterday_d, yesterday);
    storage_aggregate_quarterly(yesterday, CONSUMPTION_SOURCE);

    double baseline;
    if(storage_overnight_baseline_load_w(yesterday, &baseline)) {
        S->min_home_w = baseline < MAX_BASELINE_LOAD_W ? baseline : MAX_BASELINE_LOAD_W;
        if(baseline <= MAX_BASELINE_LOAD_W) BMS_LOGI(TAG, "min_home_w: %.1fW (overnight 01:00-03:00 average)", S->min_home_w);
        else BMS_LOGI(TAG, "min_home_w: %.1fW (capped at %.0fW, raw average was %.1fW)", S->min_home_w, MAX_BASELINE_LOAD_W, baseline);
    } else {
        S->min_home_w = DEFAULT_BASELINE_LOAD_W;
        BMS_LOGI(TAG, "min_home_w: %.1fW (default, incomplete night)", S->min_home_w);
    }

    storage_expected_consumption_curve(today, S->expected_today);
    storage_expected_consumption_curve(date_add_days(today, 1), S->expected_tomorrow);
    S->expected_quarterly_has_tomorrow = true;
    storage_actual_consumption_curve(CONSUMPTION_SOURCE, today, S->actual_today);
    storage_prune_battery_log(BATTERY_LOG_RETENTION_DAYS);
    storage_prune_measurements(MEASUREMENT_RETENTION_DAYS); // ESP32-specifiek
    refresh_solar_curves_from_storage();
    S->have_day_curves = true;
    S->last_aggregated_date = today;
}

// ---------------------------------------------------------------- replan

static void replan(double soc_pct)
{
    S->last_replan_s = now_s();
    S->replanned_once = true;

    // vandaag + morgen aan elkaar (zie het replan-blok in dashboard_daemon.py)
    static opt_d price48[48], cons192[Q2], solar48[48];
    int n_price = 0, n_cons = 0, n_solar = 0;
    if(S->price_known) {
        for(int h = 0; h < 24; h++) price48[h] = opt_some(S->price_today[h]);
        n_price = 24;
        if(S->price_tomorrow_known) {
            for(int h = 0; h < 24; h++) price48[24 + h] = opt_some(S->price_tomorrow[h]);
            n_price = 48;
        }
    }
    if(S->have_day_curves) {
        memcpy(cons192, S->expected_today, sizeof(S->expected_today));
        memcpy(cons192 + QUARTERS_PER_DAY, S->expected_tomorrow, sizeof(S->expected_tomorrow));
        n_cons = Q2;
    }
    if(S->solar_curve_known) {
        memcpy(solar48, S->solar_today, sizeof(S->solar_today));
        memcpy(solar48 + 24, S->solar_tomorrow, sizeof(S->solar_tomorrow));
        n_solar = 48;
    }

    // Echte kwartierprijzen bij elke replan opnieuw (rollend vanaf "nu").
    S->quarter_known = price_fetch_quarter_price_curve(DYNAMISCH_TARIEF_DEFAULT_HOURS_AHEAD, &S->quarter);
    if(!S->quarter_known) {
        BMS_LOGW(TAG, "dynamisch-tarief.nl quarter-price fetch failed, battery_scheduler falling back to hourly EnergyZero prices");
    }

    plan_inputs_t in = {n_price ? price48 : nullptr, n_price, n_cons ? cons192 : nullptr, n_cons,
                        n_solar ? solar48 : nullptr, n_solar,
                        S->quarter_known ? S->quarter.valid : nullptr, S->quarter.value, QUARTER_PRICE_MAX};
    plan_day(&in, soc_pct, S->battery.capacity_kwh, S->battery.max_power_w, time(nullptr), 48.0,
             S->battery.min_soc_pct, S->battery.max_soc_pct, S->battery.energy_loss_pct, &S->plan);

    // Logregel en battery_log-event, zelfde inhoud als de Pi.
    String windows, blocks;
    for(int i = 0; i < S->plan.n_windows; i++) windows += String(i ? ", " : "") + S->plan.windows_start_time[i];
    for(int i = 0; i < S->plan.n_block_windows; i++) blocks += String(i ? ", " : "") + S->plan.block_windows_start_time[i];
    BMS_LOGI(TAG, "battery_scheduler: SOC=%.2f%%, %d laadvenster(s) gekozen: [%s], %d blokvenster(s) gekozen: [%s] (%lu ms)",
             soc_pct, S->plan.n_windows, windows.c_str(), S->plan.n_block_windows, blocks.c_str(), S->plan.compute_ms);

    cJSON * detail = cJSON_CreateObject();
    cJSON_AddNumberToObject(detail, "soc_pct", soc_pct);
    cJSON * wj = cJSON_AddArrayToObject(detail, "windows_chosen");
    for(int i = 0; i < S->plan.n_windows; i++) cJSON_AddItemToArray(wj, cJSON_CreateString(S->plan.windows_start_time[i]));
    cJSON * bj = cJSON_AddArrayToObject(detail, "block_windows_chosen");
    for(int i = 0; i < S->plan.n_block_windows; i++)
        cJSON_AddItemToArray(bj, cJSON_CreateString(S->plan.block_windows_start_time[i]));
    static const char * actions[] = {nullptr, "auto", "charge", "block"};
    if(S->plan.current_decision.action == DECISION_NONE) cJSON_AddNullToObject(detail, "action");
    else cJSON_AddStringToObject(detail, "action", actions[S->plan.current_decision.action]);
    // Bodem-verwachting vandaag (zelfde als de Pi logt)
    int min_q = -1;
    double min_pct = 0;
    for(int q = 0; q < QUARTERS_PER_DAY; q++) {
        if(S->plan.soc_forecast_pct[q].valid && (min_q < 0 || S->plan.soc_forecast_pct[q].v < min_pct)) {
            min_q = q;
            min_pct = S->plan.soc_forecast_pct[q].v;
        }
    }
    if(min_q >= 0) {
        char t[6];
        snprintf(t, sizeof(t), "%02d:%02d", (min_q * 15) / 60, (min_q * 15) % 60);
        cJSON_AddNumberToObject(detail, "soc_forecast_min_today_pct", round(min_pct * 10) / 10);
        cJSON_AddStringToObject(detail, "soc_forecast_min_today_time", t);
    }
    cJSON_AddNumberToObject(detail, "shadow_soc_kwh", round(S->pool.shadow_soc_kwh * 10000) / 10000);
    storage_log_battery_event("replan", detail);
    cJSON_Delete(detail);

    if(S->plan.current_decision.action == DECISION_NONE) {
        BMS_LOGW(TAG, "battery_scheduler: te weinig data om te plannen, skip");
    } else {
        S->raw_decision = S->plan.current_decision;
        S->has_raw_decision = true;
    }
}

// ---------------------------------------------------------------- aansturing

static double monotonic_s(void)
{
    return esp_timer_get_time() / 1e6;
}

static ctrl_mode_reading_t to_reading(const marstek_mode_t * m)
{
    ctrl_mode_reading_t r = {};
    r.valid = m->valid;
    r.has_mode = m->valid && m->mode[0] != '\0';
    snprintf(r.mode, sizeof(r.mode), "%s", m->mode);
    r.ongrid_power_valid = m->ongrid_power_valid;
    r.ongrid_power = m->ongrid_power;
    return r;
}

static bool same_command(const ctrl_command_t * a, const ctrl_command_t * b)
{
    return a->kind == b->kind && a->action == b->action &&
           (a->kind != CTRL_CMD_PASSIVE || fabs(a->power_w - b->power_w) <= CTRL_POWER_DEDUP_THRESHOLD_W);
}

// Het enforce/verify-blok van de Pi, elke cyclus. In schaduw-modus (geen
// BMS_BATTERY_CONTROL_LIVE) gaat er niets naar de batterij: het commando wordt
// gelogd en als bevestigd behandeld, zodat de toestand verloopt alsof de ESP32
// stuurde. Zolang de Pi nog aanstuurt, ziet GetMode wat de Pi deed - een
// afwijkende schaduwbeslissing levert dan opnieuw-sturen op; die worden geteld
// maar niet elke cyclus in battery_log gezet.
static void control_tick(const marstek_mode_t * mode, const marstek_telemetry_t * tel, bool grid_valid, double grid_w)
{
    ctrl_mode_reading_t reading = to_reading(mode);
    if(!S->ctrl_initialized) {
        ctrl_init(&S->ctrl, &reading);
        S->ctrl_initialized = true;
        const char * last = ctrl_action_name(S->ctrl.last_sent_action);
        BMS_LOGI(TAG, "battery_control: start, actief commando volgens de batterij: %s", last ? last : "geen");
    }
    ctrl_observe_soc(&S->ctrl, tel->battery_soc_pct_valid, tel->battery_soc_pct);
    double now = monotonic_s();

    // Testvenster starten/afronden.
    if(s_test_pending) {
        portENTER_CRITICAL(&s_test_mux);
        plan_decision_t req = s_test_request;
        double seconds = s_test_request_seconds;
        s_test_pending = false;
        portEXIT_CRITICAL(&s_test_mux);
        if(req.action == DECISION_NONE) {
            S->test_until_s = now; // stop: hieronder afgerond
        } else {
            S->test_active = true;
            S->test_decision = req;
            S->test_until_s = now + seconds;
            BMS_LOGI(TAG, "battery_control TEST: %s %.0fW gedurende %.0fs", ctrl_action_name(req.action), req.power_w,
                     seconds);
            storage_log_battery_event("control_test_start", nullptr);
        }
    }
    if(S->test_active && now >= S->test_until_s) {
        S->test_active = false;
        // Geen minimale laad-/blokduur na een test: terug naar het plan.
        S->ctrl.charge_started_valid = false;
        S->ctrl.block_started_valid = false;
        BMS_LOGI(TAG, "battery_control TEST afgelopen, terug naar het plan");
        storage_log_battery_event("control_test_end", nullptr);
    }

    if(!S->battery.active_control_enabled || (!S->has_raw_decision && !S->test_active)) {
        return;
    }

    plan_decision_t decision = S->test_active ? S->test_decision : S->raw_decision;
    if(S->test_active) decision.remaining_s = S->test_until_s - now;
    ctrl_config_t cfg = {S->battery.max_power_w, S->battery.min_soc_pct, S->battery.max_soc_pct};
    ctrl_step_result_t r;
    ctrl_step(&S->ctrl, &cfg, now, &decision, &reading, grid_valid, grid_w, &r);

    if(r.charge_floor_applied) {
        BMS_LOGI(TAG, "battery_scheduler: plan wil stoppen met laden, maar minimale laadduur nog niet bereikt (%.0fs resterend) - blijf laden",
                 r.floor_remaining_s);
    }
    if(r.block_floor_applied) {
        BMS_LOGI(TAG, "battery_scheduler: plan wil stoppen met blokkeren, maar minimale blokkeerduur nog niet bereikt (%.0fs resterend) - blijf blokkeren",
                 r.floor_remaining_s);
    }
    if(r.getmode_failed) {
        BMS_LOGW(TAG, "battery_scheduler: ES.GetMode mislukt (%d op rij), kan commando niet verifiëren deze cyclus",
                 r.getmode_failures);
    }
    if(r.verify_blind) {
        BMS_LOGW(TAG, "battery_scheduler: ES.GetMode al %d cycli mislukt - commando preventief opnieuw sturen",
                 CTRL_GETMODE_FAILURE_RESEND_THRESHOLD);
    }
#if BMS_BATTERY_CONTROL_LIVE
    if(r.mismatch_event) {
        cJSON * d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "expected", r.mismatch_expected_passive ? "Passive" : "Auto");
        if(reading.has_mode) cJSON_AddStringToObject(d, "confirmed", reading.mode);
        else cJSON_AddNullToObject(d, "confirmed");
        if(r.mismatch_expected_passive) {
            cJSON_AddNumberToObject(d, "power_w", r.cmd.power_w);
            add_opt(d, "confirmed_ongrid_power", reading.ongrid_power_valid, reading.ongrid_power);
        }
        if(r.mismatch_is_power) {
            BMS_LOGW(TAG, "battery_scheduler: Passive bevestigd (actie=%s) maar ongrid_power=%.0fW (verwacht ~%.0fW) - opnieuw sturen",
                     ctrl_action_name(r.cmd.action), reading.ongrid_power, r.cmd.power_w);
        } else {
            BMS_LOGW(TAG, "battery_scheduler: commando eerder verstuurd maar batterij staat nog op %s - opnieuw sturen",
                     reading.has_mode ? reading.mode : "?");
        }
        storage_log_battery_event("mode_mismatch", d);
        cJSON_Delete(d);
    }
#endif
    if(r.cmd.kind == CTRL_CMD_NONE) {
        return;
    }

#if BMS_BATTERY_CONTROL_LIVE
    bool ack = r.cmd.kind == CTRL_CMD_AUTO ? marstek_write_auto_mode(s_marstek_host)
                                           : marstek_write_passive_mode(s_marstek_host, r.cmd.power_w, r.cmd.duration_s);
    const char * event = "command_sent";
    bool log_event = true;
#else
    bool ack = true;
    const char * event = "command_shadow";
    bool log_event = !S->have_last_cmd || !same_command(&S->last_cmd, &r.cmd);
    if(log_event) {
        BMS_LOGI(TAG, "battery_control SCHADUW: zou sturen: %s %.0fW cd_time %.0fs (niet verstuurd)",
                 ctrl_action_name(r.cmd.action), r.cmd.power_w, r.cmd.duration_s);
    }
#endif
    if(log_event) {
        cJSON * d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "action", ctrl_action_name(r.cmd.action));
        if(r.cmd.kind == CTRL_CMD_PASSIVE) {
            cJSON_AddNumberToObject(d, "power_w", r.cmd.power_w);
            cJSON_AddNumberToObject(d, "duration_s", r.cmd.duration_s);
        }
        cJSON_AddBoolToObject(d, "ack", ack);
        storage_log_battery_event(event, d);
        cJSON_Delete(d);
    }
    ctrl_on_ack(&S->ctrl, &r.cmd, ack, now);
    S->ctrl_commands++;
    S->have_last_cmd = true;
    S->last_cmd = r.cmd;
    S->last_cmd_at = time(nullptr);
}

static cJSON * control_json(void)
{
    cJSON * c = cJSON_CreateObject();
    const char * mode = !S->battery.active_control_enabled ? "off" : (BMS_BATTERY_CONTROL_LIVE ? "live" : "shadow");
    cJSON_AddStringToObject(c, "mode", mode);
    const char * last = ctrl_action_name(S->ctrl.last_sent_action);
    if(last) cJSON_AddStringToObject(c, "last_sent_action", last);
    else cJSON_AddNullToObject(c, "last_sent_action");
    add_opt(c, "last_sent_power_w", S->ctrl.last_sent_power_valid, S->ctrl.last_sent_power_w);
    cJSON_AddNumberToObject(c, "commands", S->ctrl_commands);
    if(S->test_active) {
        cJSON * t = cJSON_AddObjectToObject(c, "test");
        cJSON_AddStringToObject(t, "action", ctrl_action_name(S->test_decision.action));
        cJSON_AddNumberToObject(t, "power_w", S->test_decision.power_w);
        cJSON_AddNumberToObject(t, "remaining_s", round(S->test_until_s - monotonic_s()));
    }
    if(S->have_last_cmd) {
        cJSON * lc = cJSON_AddObjectToObject(c, "last_command");
        cJSON_AddStringToObject(lc, "action", ctrl_action_name(S->last_cmd.action));
        if(S->last_cmd.kind == CTRL_CMD_PASSIVE) {
            cJSON_AddNumberToObject(lc, "power_w", S->last_cmd.power_w);
            cJSON_AddNumberToObject(lc, "duration_s", S->last_cmd.duration_s);
        }
        char ts[32];
        struct tm tmv;
        localtime_r(&S->last_cmd_at, &tmv);
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tmv);
        cJSON_AddStringToObject(lc, "at", ts);
    }
    return c;
}

// ---------------------------------------------------------------- Pi-sturing herkennen

// Alleen-lezend: wat doet de Pi nu met de batterij? Passive + ~0 W = blok;
// Passive + laden = laadvenster, tenzij het eigen plan nu een blokvenster
// heeft (de Pi laadt dan zon-overschot op binnen een blok). Al het andere
// (Auto, geen antwoord) = de Pi stuurt niets aan.
static pi_action_t detect_pi_action(const marstek_mode_t * mode, const marstek_telemetry_t * tel)
{
    if(!mode->valid || strcmp(mode->mode, "Passive") != 0) {
        return PI_ACTION_NONE;
    }
    double power = mode->ongrid_power_valid ? mode->ongrid_power : (tel->battery_power_w_valid ? tel->battery_power_w : 0);
    if(fabs(power) <= BATTERY_POWER_DEDUP_THRESHOLD_W) {
        return PI_ACTION_BLOCK;
    }
    if(power < 0) {
        return S->plan.current_decision.action == DECISION_BLOCK ? PI_ACTION_BLOCK : PI_ACTION_CHARGE;
    }
    return PI_ACTION_NONE;
}

// ---------------------------------------------------------------- hoofdlus

static void history_append(opt_d solar, opt_d battery, opt_d grid)
{
    if(S->hist_len == HISTORY_LENGTH) {
        memmove(S->hist_solar, S->hist_solar + 1, sizeof(opt_d) * (HISTORY_LENGTH - 1));
        memmove(S->hist_battery, S->hist_battery + 1, sizeof(opt_d) * (HISTORY_LENGTH - 1));
        memmove(S->hist_grid, S->hist_grid + 1, sizeof(opt_d) * (HISTORY_LENGTH - 1));
        S->hist_len--;
    }
    S->hist_solar[S->hist_len] = solar;
    S->hist_battery[S->hist_len] = battery;
    S->hist_grid[S->hist_len] = grid;
    S->hist_len++;
}

// Logt stappen van de cyclus die ongewoon lang duren (de daemon hoort snel
// klaar te zijn; trage flash-writes lieten eerder het beeld springen).
static uint32_t s_mark_ms;
static void mark(const char * what)
{
    uint32_t now = millis();
    if(now - s_mark_ms > 5000) BMS_LOGW(TAG, "stap %s: %u ms", what, (unsigned) (now - s_mark_ms));
    s_mark_ms = millis();
}

static void tick(void)
{
    s_mark_ms = millis();
    ems_date_t today = date_today();
    if(s_reload_requested) {
        s_reload_requested = false;
        read_configs();
        // Pi herstart de daemon na opslaan -> verse zon-fetch, prijzen, replan.
        S->last_solar_fetch_s = now_s() - SOLAR_FORECAST_POLL_INTERVAL_S;
        S->last_price_fetch_s = now_s() - S->price_poll_interval_s;
        S->replanned_once = false;
        S->have_day_curves = false;
        // Na een besparingsreset: in-memory boekhouding opnieuw van de DB
        // (op de Pi gebeurt dit door de daemon-herstart).
        S->pool.shadow_soc_kwh = storage_load_pool_state();
        S->savings_cache = storage_savings_summary(date_today());
        S->have_last_savings_tick = false;
        // Op de Pi herstart de daemon: plan en aansturing beginnen opnieuw.
        S->has_raw_decision = false;
        S->ctrl_initialized = false;
    }
    if(!S->have_day_curves || !date_equal(today, S->last_aggregated_date)) {
        daily_refresh(today);
    }
    mark("daily_refresh");
    if(now_s() - S->last_solar_fetch_s >= SOLAR_FORECAST_POLL_INTERVAL_S) {
        poll_solar();
    }
    mark("poll_solar");
    if(now_s() - S->last_price_fetch_s >= S->price_poll_interval_s) {
        poll_prices();
    }
    mark("poll_prices");

    // --- weergavecurves (all-in import, export met zonnebonus) ---
    static double display_price[24], display_export[24];
    if(S->price_known) {
        for(int h = 0; h < 24; h++) {
            display_price[h] = apply_import_tariff(S->price_today[h]);
            bool zb = S->solar_curve_known && S->solar_today[h].valid && S->solar_today[h].v > 0;
            display_export[h] = apply_export_tariff(S->price_today[h], zb);
        }
    }
    static opt_d price_q[Q2], export_q[Q2];
    if(S->price_known) {
        for(int q = 0; q < Q2; q++) {
            int hour = q < 96 ? q / 4 : (q - 96) / 4;
            const double * hourly = q < 96 ? S->price_today : (S->price_tomorrow_known ? S->price_tomorrow : nullptr);
            const opt_d * solar_hourly = q < 96 ? S->solar_today : S->solar_tomorrow;
            bool have = false;
            double epex = 0;
            if(S->quarter_known && S->quarter.valid[q]) {
                epex = S->quarter.value[q];
                have = true;
            } else if(hourly) {
                epex = hourly[hour];
                have = true;
            }
            if(!have) {
                price_q[q] = export_q[q] = opt_none();
                continue;
            }
            price_q[q] = opt_some(apply_import_tariff(epex));
            bool zb = S->solar_curve_known && solar_hourly[hour].valid && solar_hourly[hour].v > 0;
            export_q[q] = opt_some(apply_export_tariff(epex, zb));
        }
    }

    // --- data_sources.build_message(): telemetrie + P1 ---
    marstek_telemetry_t tel = marstek_read_telemetry(s_marstek_host);
    mark("marstek_telemetry");
    double p1_w;
    bool grid_valid;
    double grid_w;
    if(p1_read_grid_power_w(&p1_w)) {
        grid_valid = true;
        grid_w = p1_w;
    } else {
        BMS_LOGI(TAG, "HomeWizard P1 meter unavailable, falling back to Marstek's own grid estimate");
        grid_valid = tel.grid_power_w_valid;
        grid_w = tel.grid_power_w;
    }
    mark("p1");
    history_append(tel.solar_power_w_valid ? opt_some(tel.solar_power_w) : opt_none(),
                   tel.battery_power_w_valid ? opt_some(tel.battery_power_w) : opt_none(),
                   grid_valid ? opt_some(grid_w) : opt_none());

    struct tm local;
    time_t now = time(nullptr);
    localtime_r(&now, &local);

    cJSON * m = cJSON_CreateObject();
    char ts[48];
    iso_utc_now(ts, sizeof(ts));
    cJSON_AddStringToObject(m, "timestamp", ts);
    add_opt(m, "battery_power_w", tel.battery_power_w_valid, tel.battery_power_w);
    if(tel.battery_state) cJSON_AddStringToObject(m, "battery_state", tel.battery_state);
    else cJSON_AddNullToObject(m, "battery_state");
    add_opt(m, "battery_soc_pct", tel.battery_soc_pct_valid, tel.battery_soc_pct);
    add_opt(m, "solar_power_w", tel.solar_power_w_valid, tel.solar_power_w);
    add_opt(m, "grid_power_w", grid_valid, grid_w);
    add_opt(m, "price_now", S->price_known, display_price[local.tm_hour]);
    add_double_array(m, "price_curve", S->price_known, display_price, 24);
    cJSON * hist = cJSON_AddObjectToObject(m, "power_history");
    cJSON_AddItemToObject(hist, "solar", opt_array(S->hist_solar, S->hist_len));
    cJSON_AddItemToObject(hist, "battery", opt_array(S->hist_battery, S->hist_len));
    cJSON_AddItemToObject(hist, "grid", opt_array(S->hist_grid, S->hist_len));

    add_double_array(m, "export_price_curve", S->price_known, display_export, 24);
    add_opt_array(m, "price_curve_quarterly", S->price_known, price_q, Q2);
    add_opt_array(m, "export_price_curve_quarterly", S->price_known, export_q, Q2);
    add_opt_array(m, "expected_consumption_curve", S->have_day_curves, S->expected_today, QUARTERS_PER_DAY);
    add_opt_array(m, "actual_consumption_curve", S->have_day_curves, S->actual_today, QUARTERS_PER_DAY);
    if(S->have_day_curves) {
        cJSON * eq = opt_array(S->expected_today, QUARTERS_PER_DAY);
        for(int q = 0; q < QUARTERS_PER_DAY; q++)
            cJSON_AddItemToArray(eq, S->expected_tomorrow[q].valid ? cJSON_CreateNumber(S->expected_tomorrow[q].v)
                                                                  : cJSON_CreateNull());
        cJSON_AddItemToObject(m, "expected_consumption_curve_quarterly", eq);
    } else {
        cJSON_AddNullToObject(m, "expected_consumption_curve_quarterly");
    }
    add_opt_array(m, "solar_forecast_curve", S->solar_curve_known, S->solar_today, 24);
    if(S->solar_curve_known) {
        cJSON * sq = opt_array(S->solar_q_today, QUARTERS_PER_DAY);
        for(int q = 0; q < QUARTERS_PER_DAY; q++)
            cJSON_AddItemToArray(sq, S->solar_q_tomorrow[q].valid ? cJSON_CreateNumber(S->solar_q_tomorrow[q].v)
                                                                 : cJSON_CreateNull());
        cJSON_AddItemToObject(m, "solar_forecast_curve_quarterly", sq);
    } else {
        cJSON_AddNullToObject(m, "solar_forecast_curve_quarterly");
    }
    add_opt(m, "solar_kwp", S->solar_kwp_known && !S->no_solar_panels, S->solar_kwp);
    cJSON_AddNumberToObject(m, "battery_capacity_kwh", S->battery.capacity_kwh);
    cJSON_AddNumberToObject(m, "min_home_w", S->min_home_w);
    cJSON_AddBoolToObject(m, "gas_enabled", S->gas.enabled);
    cJSON_AddStringToObject(m, "gas_price_unit", S->gas.unit_is_m3 ? "EUR/m3" : "EUR/kWh");
    add_double_array(m, "gas_price_curve", S->gas.enabled && S->gas_known, S->gas_curve, 24);
    cJSON_AddItemToObject(m, "settings", settings_json());

    double pend_b, pend_e;
    storage_savings_pending(&pend_b, &pend_e);
    mark("json+savings_pending");
    const struct {
        const char * name;
        savings_pair_t p;
    } periods[] = {{"dag", S->savings_cache.dag},
                   {"week", S->savings_cache.week},
                   {"maand", S->savings_cache.maand},
                   {"jaar", S->savings_cache.jaar},
                   {"altijd", S->savings_cache.altijd}};
    for(auto & p : periods) {
        char k[40];
        snprintf(k, sizeof(k), "savings_%s_batterij_eur", p.name);
        cJSON_AddNumberToObject(m, k, p.p.batterij_eur + pend_b);
        snprintf(k, sizeof(k), "savings_%s_ems_eur", p.name);
        cJSON_AddNumberToObject(m, k, p.p.ems_eur + pend_e);
    }

    // --- replan (elke 5 min, alleen met verse SOC) ---
    if(!S->replanned_once || now_s() - S->last_replan_s >= REPLAN_INTERVAL_S) {
        if(!tel.battery_soc_pct_valid) {
            BMS_LOGW(TAG, "battery_scheduler: no fresh SOC reading, skipping this replan");
        } else {
            replan(tel.battery_soc_pct);
        }
    }
    mark("replan");

    // --- Pi-sturing herkennen (alleen lezen) ---
    marstek_mode_t mode = marstek_get_mode(s_marstek_host);
    S->pi_action = detect_pi_action(&mode, &tel);
    mark("get_mode");
    control_tick(&mode, &tel, grid_valid, grid_w);
    mark("control");

    cJSON_AddItemToObject(m, "battery_plan_eigen_zon_wh", opt_array(S->plan.eigen_zon_wh, Q2));
    cJSON_AddItemToObject(m, "battery_plan_actief_gestuurd_wh", opt_array(S->plan.actief_gestuurd_wh, Q2));
    cJSON_AddItemToObject(m, "battery_plan_verwacht_verbruik_wh", opt_array(S->plan.verwacht_verbruik_wh, Q2));
    cJSON_AddItemToObject(m, "battery_plan_geblokkeerd_wh", opt_array(S->plan.geblokkeerd_wh, Q2));
    cJSON_AddItemToObject(m, "battery_soc_forecast_pct", opt_array(S->plan.soc_forecast_pct, Q2));
    // Alleen een LIVE-build stuurt echt aan - zie daemon.h.
    cJSON_AddBoolToObject(m, "battery_active_control_enabled",
                          BMS_BATTERY_CONTROL_LIVE && S->battery.active_control_enabled);
    // ESP32-extra: wat de aansturing doet of (schaduw) zou doen.
    cJSON_AddItemToObject(m, "battery_control", control_json());
    // ESP32-extra (niet op de Pi): wat de ESP32 alleen-lezend ziet dat de Pi doet.
    static const char * pi_names[] = {"none", "charge", "block"};
    cJSON_AddStringToObject(m, "pi_battery_action", pi_names[S->pi_action]);

    char * text = cJSON_PrintUnformatted(m);
    if(text) {
        state_bus_publish(text);
        free(text);
    }
    cJSON_Delete(m);
    mark("publish");

    // --- metingen bufferen ---
    if(tel.battery_power_w_valid) storage_measurement_record("battery_power", tel.battery_power_w, "W");
    if(tel.solar_power_w_valid) storage_measurement_record("solar_power", tel.solar_power_w, "W");
    if(grid_valid) storage_measurement_record("grid_power", grid_w, "W");
    if(tel.battery_soc_pct_valid) storage_measurement_record("battery_soc_pct", tel.battery_soc_pct, "%");
    mark("measurement_record");

    // --- household_load + besparingen ---
    bool solar_est_valid = false;
    double solar_estimate_w = 0.0;
    if(S->solar_curve_known) {
        solar_est_valid = true;
        int h = local.tm_hour;
        if(S->solar_today[h].valid) {
            solar_estimate_w = S->solar_today[h].v * 1000.0;
            if(h + 1 < 24 && S->solar_today[h + 1].valid) {
                double next = S->solar_today[h + 1].v * 1000.0;
                double frac = (local.tm_min / 5 * 5) / 60.0;
                solar_estimate_w += (next - solar_estimate_w) * frac;
            }
        }
    }

    if(solar_est_valid && tel.battery_power_w_valid && grid_valid) {
        double raw_load = solar_estimate_w + tel.battery_power_w + grid_w;
        double load = solar_estimate_w > 0 ? (raw_load > S->min_home_w ? raw_load : S->min_home_w) : raw_load;
        storage_measurement_record("household_load", load, "W");

        int cq = (local.tm_hour * 60 + local.tm_min) / 15;
        if(S->price_known && price_q[cq].valid && export_q[cq].valid) {
            double import_price = price_q[cq].v, export_price = export_q[cq].v;
            uint32_t now_ms = millis();
            double elapsed_s = PUSH_INTERVAL_S;
            if(S->have_last_savings_tick) {
                elapsed_s = (now_ms - S->last_savings_tick_ms) / 1000.0;
                if(elapsed_s > MAX_SAVINGS_GAP_S) elapsed_s = MAX_SAVINGS_GAP_S;
            }
            S->have_last_savings_tick = true;
            S->last_savings_tick_ms = now_ms;

            double p = tel.battery_power_w;
            double kwh = fabs(p) * elapsed_s / 3600.0 / 1000.0;
            if(p < -SAVINGS_IDLE_DEADBAND_W) {
                double surplus_w = solar_estimate_w - raw_load;
                if(surplus_w < 0) surplus_w = 0;
                double surplus_kwh = surplus_w * elapsed_s / 3600.0 / 1000.0;
                // Pi: battery_active_control_enabled and last_sent_action == "charge"
#if BMS_BATTERY_CONTROL_LIVE
                bool active_window = S->battery.active_control_enabled && S->ctrl.last_sent_action == DECISION_CHARGE;
#else
                bool active_window = S->pi_action == PI_ACTION_CHARGE;
#endif
                charge_result_t r = savings_charge(S->pool, kwh, S->battery.capacity_kwh, surplus_kwh, export_price,
                                                   import_price, active_window);
                S->pool = r.state;
                storage_savings_record(r.batterij_cash_flow_eur, r.ems_cash_flow_eur);
            } else if(p > SAVINGS_IDLE_DEADBAND_W) {
                discharge_result_t r = savings_discharge(S->pool, kwh, import_price);
                S->pool = r.state;
                storage_savings_record(r.batterij_savings_eur, r.ems_savings_eur);
#if BMS_BATTERY_CONTROL_LIVE
            } else if(S->battery.active_control_enabled && S->ctrl.last_sent_action == DECISION_BLOCK) {
#else
            } else if(S->pi_action == PI_ACTION_BLOCK) {
#endif
                if(grid_w > 0) {
                    S->pool = savings_block(S->pool, grid_w * elapsed_s / 3600.0 / 1000.0);
                }
            }
        }
    }

    // --- flush (elke 60 s) ---
    if(now_s() - S->last_flush_s >= FLUSH_INTERVAL_S) {
        storage_measurement_flush();
        mark("measurement_flush");
        S->last_flush_s = now_s();
        cJSON * stats = marstek_get_call_stats();
        if(stats) {
            storage_log_battery_event("udp_call_stats", stats);
            cJSON_Delete(stats);
        }
        marstek_reset_call_stats();
        mark("udp_stats_log");
        storage_actual_consumption_curve(CONSUMPTION_SOURCE, date_today(), S->actual_today);
        mark("actual_curve");
        storage_save_pool_state(S->pool.shadow_soc_kwh);
        mark("pool_state");
        storage_savings_flush();
        mark("savings_flush");
        S->savings_cache = storage_savings_summary(date_today());
        mark("savings_summary");
        storage_persist_if_due();
        mark("persist");
    }
}

// Planner-zoeklus: elke ~50 ms rekenen een tick afstaan (zie sched_set_yield_hook).
static void planner_yield(void)
{
    static uint32_t s_last = 0;
    if(millis() - s_last >= 50) {
        vTaskDelay(1);
        s_last = millis();
    }
}

static void daemon_task(void * arg)
{
    (void) arg;
    sched_set_yield_hook(planner_yield);
    // Alles per dag/uur heeft een geldige klok nodig.
    while(!net_wait_for_time(60000)) {
        BMS_LOGW(TAG, "nog geen NTP-tijd, daemon wacht");
    }

    read_configs();
    S->min_home_w = DEFAULT_BASELINE_LOAD_W;
    S->pool.shadow_soc_kwh = storage_load_pool_state();
    S->savings_cache = storage_savings_summary(date_today());
    S->price_poll_interval_s = PRICE_POLL_INTERVAL_S;
    // "now - interval"-truc: eerste iteratie haalt meteen alles op.
    S->last_solar_fetch_s = now_s() - SOLAR_FORECAST_POLL_INTERVAL_S;
    S->last_price_fetch_s = now_s() - PRICE_POLL_INTERVAL_S;
    S->last_flush_s = now_s();
    opt_fill_none(S->plan.eigen_zon_wh, Q2);
    opt_fill_none(S->plan.actief_gestuurd_wh, Q2);
    opt_fill_none(S->plan.verwacht_verbruik_wh, Q2);
    opt_fill_none(S->plan.geblokkeerd_wh, Q2);
    opt_fill_none(S->plan.soc_forecast_pct, Q2);

    for(;;) {
        uint32_t started = millis();
        tick();
        uint32_t took = millis() - started;
        uint32_t jitter_ms = 200 + esp_random() % (uint32_t) (JITTER_CAP_S * 1000 - 200);
        // Pi slaapt PUSH_INTERVAL_S NA elke cyclus; hier idem.
        BMS_LOGI(TAG, "cyclus klaar in %u ms", (unsigned) took);
        vTaskDelay(pdMS_TO_TICKS(PUSH_INTERVAL_S * 1000 + jitter_ms));
    }
}

bool daemon_control_test(const char * action, double power_w, double seconds, char * error_out,
                         unsigned error_out_size)
{
    if(!BMS_BATTERY_CONTROL_LIVE) {
        snprintf(error_out, error_out_size, "deze firmware stuurt de batterij niet aan (schaduw-modus)");
        return false;
    }
    if(!S || !S->battery.active_control_enabled) {
        snprintf(error_out, error_out_size, "aansturing staat uit in de batterij-instellingen");
        return false;
    }
    plan_decision_t d = {DECISION_NONE, 0.0, 0.0};
    if(strcmp(action, "charge") == 0) {
        d.action = DECISION_CHARGE;
        d.power_w = isnan(power_w) ? -S->battery.max_power_w : power_w;
        if(d.power_w >= 0 || -d.power_w > S->battery.max_power_w) {
            snprintf(error_out, error_out_size, "laadvermogen moet tussen -%.0f en 0 W liggen", S->battery.max_power_w);
            return false;
        }
    } else if(strcmp(action, "block") == 0) {
        d.action = DECISION_BLOCK;
    } else if(strcmp(action, "auto") == 0) {
        d.action = DECISION_AUTO;
    } else if(strcmp(action, "stop") != 0) {
        snprintf(error_out, error_out_size, "onbekende actie (charge/block/auto/stop): %s", action);
        return false;
    }
    if(d.action != DECISION_NONE && !(seconds >= 30 && seconds <= 1800)) {
        snprintf(error_out, error_out_size, "duur moet 30-1800 s zijn");
        return false;
    }
    portENTER_CRITICAL(&s_test_mux);
    s_test_request = d;
    s_test_request_seconds = seconds;
    s_test_pending = true;
    portEXIT_CRITICAL(&s_test_mux);
    return true;
}

void daemon_start(void)
{
    S = (daemon_state_t *) heap_caps_calloc(1, sizeof(daemon_state_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!S) {
        BMS_LOGE(TAG, "geen geheugen voor daemon-toestand");
        return;
    }
    // Ruime stack: cJSON-opbouw, HTTPS (mbedTLS) en de planner draaien hierin.
    xTaskCreatePinnedToCore(daemon_task, "daemon", 16384, nullptr, 1, nullptr, 0);
}
