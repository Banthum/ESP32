#include "ipc_client.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "cJSON.h"
#include "../core/state_bus.h"

/* dashboard_state_t is ~25 KB (vele 192-punts arrays) - op de ESP32 te groot
 * voor een stack of het interne RAM, dus alle drie de exemplaren (de
 * gedeelde toestand, de parse-buffer en de kopie voor de LVGL-taak) staan in
 * PSRAM, eenmalig gealloceerd. */
static pthread_mutex_t g_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static dashboard_state_t * g_state;    /* connected=false, alle *_valid=false (calloc) */
static dashboard_state_t * g_snapshot; /* alleen gebruikt door handle_line() (poll-taak) */
static dashboard_state_t * g_view;     /* alleen gebruikt door ipc_client_get_state() (LVGL-taak) */

static dashboard_state_t * alloc_state(void)
{
    return (dashboard_state_t *) heap_caps_calloc(1, sizeof(dashboard_state_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void ipc_client_init(void)
{
    if(!g_state) {
        g_state = alloc_state();
        g_snapshot = alloc_state();
        g_view = alloc_state();
    }
}

static void set_connected(bool connected)
{
    pthread_mutex_lock(&g_state_mutex);
    g_state->connected = connected;
    pthread_mutex_unlock(&g_state_mutex);
}

static double json_number_or(cJSON * obj, double fallback, bool * out_valid)
{
    if(obj == NULL || !cJSON_IsNumber(obj)) {
        *out_valid = false;
        return fallback;
    }
    *out_valid = true;
    return obj->valuedouble;
}

static void copy_string_field(cJSON * obj, char * dest, size_t dest_size, bool * out_valid)
{
    if(obj == NULL || !cJSON_IsString(obj)) {
        *out_valid = false;
        dest[0] = '\0';
        return;
    }
    *out_valid = true;
    snprintf(dest, dest_size, "%s", obj->valuestring);
}

static void copy_history_series(cJSON * arr, double * dest, int max_len, int * out_len)
{
    if(arr == NULL || !cJSON_IsArray(arr)) {
        *out_len = 0;
        return;
    }
    int n = cJSON_GetArraySize(arr);
    if(n > max_len) n = max_len;
    for(int i = 0; i < n; i++) {
        cJSON * item = cJSON_GetArrayItem(arr, i);
        dest[i] = cJSON_IsNumber(item) ? item->valuedouble : 0.0;
    }
    *out_len = n;
}

/* Unlike copy_history_series() above, each element gets its own validity
 * bit: expected_consumption_curve/solar_forecast_curve can have some
 * slots present and others `null` in the same array (see
 * docs/ipc_protocol.md) — a single "got this field at all" flag isn't
 * enough here. `dest_valid`/`dest` must both have room for `expected_len`
 * elements (fixed at compile time, unlike copy_history_series()'s
 * runtime-variable length). */
static void copy_nullable_series(cJSON * arr, bool * dest_valid, double * dest, int expected_len)
{
    if(arr == NULL || !cJSON_IsArray(arr) || cJSON_GetArraySize(arr) != expected_len) {
        return; /* dest_valid[] stays all-false from the caller's memset */
    }
    for(int i = 0; i < expected_len; i++) {
        cJSON * item = cJSON_GetArrayItem(arr, i);
        if(cJSON_IsNumber(item)) {
            dest_valid[i] = true;
            dest[i] = item->valuedouble;
        }
    }
}

/* Parses one complete JSON line into a fresh local snapshot, then swaps it
 * into g_state under the mutex — a malformed/partial line just gets
 * dropped (next push arrives within PUSH_INTERVAL_S regardless, see
 * docs/ipc_protocol.md's "no deltas" design), never crashes the reader. */
static void handle_line(const char * line)
{
    cJSON * root = cJSON_Parse(line);
    if(root == NULL) {
        return;
    }

    /* `snapshot` is hier een alias voor de PSRAM-buffer (zie g_snapshot),
     * zodat de rest van deze functie letterlijk gelijk blijft aan de Pi. */
#define snapshot (*g_snapshot)
    memset(g_snapshot, 0, sizeof(dashboard_state_t));
    snapshot.connected = true;

    snapshot.battery_power_w = json_number_or(cJSON_GetObjectItemCaseSensitive(root, "battery_power_w"),
                                               0.0, &snapshot.battery_power_w_valid);
    copy_string_field(cJSON_GetObjectItemCaseSensitive(root, "battery_state"),
                       snapshot.battery_state, sizeof(snapshot.battery_state), &snapshot.battery_state_valid);
    {
        bool valid;
        double v = json_number_or(cJSON_GetObjectItemCaseSensitive(root, "battery_soc_pct"), 0.0, &valid);
        snapshot.battery_soc_pct_valid = valid;
        snapshot.battery_soc_pct = (int) v;
    }
    snapshot.solar_power_w = json_number_or(cJSON_GetObjectItemCaseSensitive(root, "solar_power_w"),
                                             0.0, &snapshot.solar_power_w_valid);
    snapshot.grid_power_w = json_number_or(cJSON_GetObjectItemCaseSensitive(root, "grid_power_w"),
                                            0.0, &snapshot.grid_power_w_valid);
    snapshot.price_now = json_number_or(cJSON_GetObjectItemCaseSensitive(root, "price_now"),
                                         0.0, &snapshot.price_now_valid);

    cJSON * price_curve = cJSON_GetObjectItemCaseSensitive(root, "price_curve");
    if(price_curve != NULL && cJSON_IsArray(price_curve) &&
       cJSON_GetArraySize(price_curve) == IPC_PRICE_CURVE_LEN) {
        snapshot.price_curve_valid = true;
        for(int i = 0; i < IPC_PRICE_CURVE_LEN; i++) {
            cJSON * item = cJSON_GetArrayItem(price_curve, i);
            snapshot.price_curve[i] = cJSON_IsNumber(item) ? item->valuedouble : 0.0;
        }
    }

    cJSON * export_price_curve = cJSON_GetObjectItemCaseSensitive(root, "export_price_curve");
    if(export_price_curve != NULL && cJSON_IsArray(export_price_curve) &&
       cJSON_GetArraySize(export_price_curve) == IPC_PRICE_CURVE_LEN) {
        snapshot.export_price_curve_valid = true;
        for(int i = 0; i < IPC_PRICE_CURVE_LEN; i++) {
            cJSON * item = cJSON_GetArrayItem(export_price_curve, i);
            snapshot.export_price_curve[i] = cJSON_IsNumber(item) ? item->valuedouble : 0.0;
        }
    }

    copy_nullable_series(cJSON_GetObjectItemCaseSensitive(root, "price_curve_quarterly"),
                          snapshot.price_curve_quarterly_valid,
                          snapshot.price_curve_quarterly, IPC_BATTERY_PLAN_LEN);
    copy_nullable_series(cJSON_GetObjectItemCaseSensitive(root, "export_price_curve_quarterly"),
                          snapshot.export_price_curve_quarterly_valid,
                          snapshot.export_price_curve_quarterly, IPC_BATTERY_PLAN_LEN);

    cJSON * gas_enabled = cJSON_GetObjectItemCaseSensitive(root, "gas_enabled");
    snapshot.gas_enabled = cJSON_IsBool(gas_enabled) && cJSON_IsTrue(gas_enabled);
    bool gas_unit_valid; /* unused - gas_price_unit has no separate _valid field, see ipc_client.h */
    copy_string_field(cJSON_GetObjectItemCaseSensitive(root, "gas_price_unit"), snapshot.gas_price_unit,
                       sizeof(snapshot.gas_price_unit), &gas_unit_valid);
    cJSON * gas_price_curve = cJSON_GetObjectItemCaseSensitive(root, "gas_price_curve");
    if(gas_price_curve != NULL && cJSON_IsArray(gas_price_curve) &&
       cJSON_GetArraySize(gas_price_curve) == IPC_PRICE_CURVE_LEN) {
        snapshot.gas_price_curve_valid = true;
        for(int i = 0; i < IPC_PRICE_CURVE_LEN; i++) {
            cJSON * item = cJSON_GetArrayItem(gas_price_curve, i);
            snapshot.gas_price_curve[i] = cJSON_IsNumber(item) ? item->valuedouble : 0.0;
        }
    }

    cJSON * history = cJSON_GetObjectItemCaseSensitive(root, "power_history");
    if(history != NULL) {
        int solar_len, battery_len, grid_len;
        copy_history_series(cJSON_GetObjectItemCaseSensitive(history, "solar"),
                             snapshot.power_history_solar, IPC_HISTORY_MAX, &solar_len);
        copy_history_series(cJSON_GetObjectItemCaseSensitive(history, "battery"),
                             snapshot.power_history_battery, IPC_HISTORY_MAX, &battery_len);
        copy_history_series(cJSON_GetObjectItemCaseSensitive(history, "grid"),
                             snapshot.power_history_grid, IPC_HISTORY_MAX, &grid_len);
        /* Series always pushed with matching lengths (see data_sources.py's
         * HistoryBuffer) — smallest wins if a client somehow raced a resize. */
        snapshot.power_history_len = solar_len;
        if(battery_len < snapshot.power_history_len) snapshot.power_history_len = battery_len;
        if(grid_len < snapshot.power_history_len) snapshot.power_history_len = grid_len;
    }

    copy_nullable_series(cJSON_GetObjectItemCaseSensitive(root, "expected_consumption_curve"),
                          snapshot.expected_consumption_curve_valid,
                          snapshot.expected_consumption_curve, IPC_QUARTERS_PER_DAY);
    copy_nullable_series(cJSON_GetObjectItemCaseSensitive(root, "actual_consumption_curve"),
                          snapshot.actual_consumption_curve_valid,
                          snapshot.actual_consumption_curve, IPC_QUARTERS_PER_DAY);
    copy_nullable_series(cJSON_GetObjectItemCaseSensitive(root, "expected_consumption_curve_quarterly"),
                          snapshot.expected_consumption_curve_quarterly_valid,
                          snapshot.expected_consumption_curve_quarterly, IPC_BATTERY_PLAN_LEN);
    copy_nullable_series(cJSON_GetObjectItemCaseSensitive(root, "solar_forecast_curve"),
                          snapshot.solar_forecast_curve_valid,
                          snapshot.solar_forecast_curve, IPC_SOLAR_FORECAST_LEN);
    copy_nullable_series(cJSON_GetObjectItemCaseSensitive(root, "solar_forecast_curve_quarterly"),
                          snapshot.solar_forecast_curve_quarterly_valid,
                          snapshot.solar_forecast_curve_quarterly, IPC_BATTERY_PLAN_LEN);
    snapshot.solar_kwp = json_number_or(cJSON_GetObjectItemCaseSensitive(root, "solar_kwp"),
                                         0.0, &snapshot.solar_kwp_valid);
    snapshot.battery_capacity_kwh = json_number_or(cJSON_GetObjectItemCaseSensitive(root, "battery_capacity_kwh"),
                                                    0.0, &snapshot.battery_capacity_kwh_valid);
    /* 100.0 fallback (not 0.0 like the other json_number_or() calls here)
     * - this value is a floor, so a missing/malformed field (e.g. an
     * older daemon build without this field yet) must still fall back to
     * a sane non-zero floor, not silently disable the floor entirely. */
    snapshot.min_home_w = json_number_or(cJSON_GetObjectItemCaseSensitive(root, "min_home_w"),
                                          100.0, &snapshot.min_home_w_valid);

    copy_nullable_series(cJSON_GetObjectItemCaseSensitive(root, "battery_plan_eigen_zon_wh"),
                          snapshot.battery_plan_eigen_zon_wh_valid,
                          snapshot.battery_plan_eigen_zon_wh, IPC_BATTERY_PLAN_LEN);
    copy_nullable_series(cJSON_GetObjectItemCaseSensitive(root, "battery_plan_actief_gestuurd_wh"),
                          snapshot.battery_plan_actief_gestuurd_wh_valid,
                          snapshot.battery_plan_actief_gestuurd_wh, IPC_BATTERY_PLAN_LEN);
    copy_nullable_series(cJSON_GetObjectItemCaseSensitive(root, "battery_plan_verwacht_verbruik_wh"),
                          snapshot.battery_plan_verwacht_verbruik_wh_valid,
                          snapshot.battery_plan_verwacht_verbruik_wh, IPC_BATTERY_PLAN_LEN);
    copy_nullable_series(cJSON_GetObjectItemCaseSensitive(root, "battery_plan_geblokkeerd_wh"),
                          snapshot.battery_plan_geblokkeerd_wh_valid,
                          snapshot.battery_plan_geblokkeerd_wh, IPC_BATTERY_PLAN_LEN);
    copy_nullable_series(cJSON_GetObjectItemCaseSensitive(root, "battery_soc_forecast_pct"),
                          snapshot.battery_soc_forecast_pct_valid,
                          snapshot.battery_soc_forecast_pct, IPC_BATTERY_PLAN_LEN);
    cJSON * battery_active_control_enabled = cJSON_GetObjectItemCaseSensitive(root, "battery_active_control_enabled");
    snapshot.battery_active_control_enabled =
        cJSON_IsBool(battery_active_control_enabled) && cJSON_IsTrue(battery_active_control_enabled);

    /* Savings counters (2026-10-03) - 10 fields always sent together, one
     * shared _valid flag (see ipc_client.h). json_number_or() on the
     * first field also sets savings_valid; the other 9 reuse a local
     * throwaway bool since they're only ever read/trusted via that first
     * flag, same pattern copy_nullable_series() callers use for an
     * all-or-nothing array. */
    bool unused_valid;
    snapshot.savings_dag_batterij_eur = json_number_or(
        cJSON_GetObjectItemCaseSensitive(root, "savings_dag_batterij_eur"), 0.0, &snapshot.savings_valid);
    snapshot.savings_dag_ems_eur = json_number_or(
        cJSON_GetObjectItemCaseSensitive(root, "savings_dag_ems_eur"), 0.0, &unused_valid);
    snapshot.savings_week_batterij_eur = json_number_or(
        cJSON_GetObjectItemCaseSensitive(root, "savings_week_batterij_eur"), 0.0, &unused_valid);
    snapshot.savings_week_ems_eur = json_number_or(
        cJSON_GetObjectItemCaseSensitive(root, "savings_week_ems_eur"), 0.0, &unused_valid);
    snapshot.savings_maand_batterij_eur = json_number_or(
        cJSON_GetObjectItemCaseSensitive(root, "savings_maand_batterij_eur"), 0.0, &unused_valid);
    snapshot.savings_maand_ems_eur = json_number_or(
        cJSON_GetObjectItemCaseSensitive(root, "savings_maand_ems_eur"), 0.0, &unused_valid);
    snapshot.savings_jaar_batterij_eur = json_number_or(
        cJSON_GetObjectItemCaseSensitive(root, "savings_jaar_batterij_eur"), 0.0, &unused_valid);
    snapshot.savings_jaar_ems_eur = json_number_or(
        cJSON_GetObjectItemCaseSensitive(root, "savings_jaar_ems_eur"), 0.0, &unused_valid);
    snapshot.savings_altijd_batterij_eur = json_number_or(
        cJSON_GetObjectItemCaseSensitive(root, "savings_altijd_batterij_eur"), 0.0, &unused_valid);
    snapshot.savings_altijd_ems_eur = json_number_or(
        cJSON_GetObjectItemCaseSensitive(root, "savings_altijd_ems_eur"), 0.0, &unused_valid);

    cJSON_Delete(root);

    pthread_mutex_lock(&g_state_mutex);
    memcpy(g_state, g_snapshot, sizeof(dashboard_state_t));
    pthread_mutex_unlock(&g_state_mutex);
#undef snapshot
}

/* ESP32: geen Unix-socket meer - de daemon draait in hetzelfde proces en
 * publiceert elk volledig bericht op de state bus (core/state_bus.h). Deze
 * taak pakt elke nieuwe versie op en parseert die met dezelfde handle_line()
 * als op de Pi. "connected" gaat uit als er STALE_AFTER_S geen nieuw bericht
 * kwam (zelfde "Wachten op data..."-gedrag als bij een weggevallen daemon). */
#define POLL_INTERVAL_MS 1000
#define STALE_AFTER_S 120

static void poll_task(void * arg)
{
    (void) arg;
    uint32_t last_version = 0;
    TickType_t last_update = xTaskGetTickCount();
    for(;;) {
        uint32_t version = state_bus_version();
        if(version != last_version) {
            char * json = state_bus_copy(&version);
            if(json) {
                handle_line(json);
                free(json);
            }
            last_version = version;
            last_update = xTaskGetTickCount();
        } else if((xTaskGetTickCount() - last_update) > pdMS_TO_TICKS(STALE_AFTER_S * 1000)) {
            set_connected(false);
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

void ipc_client_start(void)
{
    xTaskCreatePinnedToCore(poll_task, "ui_state", 6144, NULL, 1, NULL, 0);
}

const dashboard_state_t * ipc_client_get_state(void)
{
    /* Stabiele kopie voor de LVGL-taak (die enige aanroeper is), zodat een
     * gelijktijdige update halverwege het tekenen niets verandert. */
    pthread_mutex_lock(&g_state_mutex);
    memcpy(g_view, g_state, sizeof(dashboard_state_t));
    pthread_mutex_unlock(&g_state_mutex);
    return g_view;
}
