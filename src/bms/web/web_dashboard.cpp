#include <Arduino.h>

#include "web_dashboard.h"

#include <cJSON.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../config/config.h"
#include "../config/solar_settings.h"
#include "../core/log.h"
#include "../core/paths.h"
#include "../core/state_bus.h"
#include "../daemon/daemon.h"
#include "../storage/storage.h"
#include "web_assets.h"

static const char * TAG = "dashboard.web_server";
static WebServer * s_srv = nullptr;

// web_server.py: MAX_REQUEST_BODY_BYTES
static const size_t MAX_REQUEST_BODY_BYTES = 8192;

// web_server.py: BATTERY_FIELDS - de Home Assistant-export (ha.html).
static const char * BATTERY_FIELDS[] = {
    "battery_power_w",
    "battery_state",
    "battery_soc_pct",
    "battery_capacity_kwh",
    "battery_active_control_enabled",
    "battery_plan_eigen_zon_wh",
    "battery_plan_actief_gestuurd_wh",
    "battery_plan_verwacht_verbruik_wh",
    "battery_plan_geblokkeerd_wh",
    "battery_soc_forecast_pct",
};

static void send_body(int code, const char * type, const char * body, const char * cache)
{
    s_srv->sendHeader("Cache-Control", cache);
    s_srv->send(code, type, body);
}

static void send_json(int code, cJSON * json)
{
    char * text = cJSON_PrintUnformatted(json);
    send_body(code, "application/json", text ? text : "null", "no-store");
    free(text);
}

static void send_result(int code, bool ok, const char * error)
{
    cJSON * j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", ok);
    if(!ok) cJSON_AddStringToObject(j, "error", error);
    send_json(code, j);
    cJSON_Delete(j);
}

// Huidige toestand (zelfde bericht als /api/state), of NULL zolang er nog niets is.
static cJSON * latest_state(void)
{
    char * text = state_bus_copy(nullptr);
    if(!text) return nullptr;
    cJSON * state = cJSON_Parse(text);
    free(text);
    return state;
}

// ---------------------------------------------------------------- GET

static void handle_battery(void)
{
    cJSON * state = latest_state();
    if(!state) {
        send_body(200, "application/json", "null", "no-store");
        return;
    }
    cJSON * out = cJSON_CreateObject();
    for(const char * field : BATTERY_FIELDS) {
        cJSON * v = cJSON_GetObjectItemCaseSensitive(state, field);
        cJSON_AddItemToObject(out, field, v ? cJSON_Duplicate(v, true) : cJSON_CreateNull());
    }
    send_json(200, out);
    cJSON_Delete(out);
    cJSON_Delete(state);
}

static void handle_settings(void)
{
    cJSON * state = latest_state();
    if(!state) {
        send_body(200, "application/json", "null", "no-store");
        return;
    }
    cJSON * s = cJSON_GetObjectItemCaseSensitive(state, "settings");
    if(s) send_json(200, s);
    else send_body(200, "application/json", "null", "no-store");
    cJSON_Delete(state);
}

static void send_asset(const web_asset_t * a)
{
    s_srv->sendHeader("Cache-Control", "no-cache");
    s_srv->send_P(200, a->content_type, (const char *) a->data, a->size);
}

// ---------------------------------------------------------------- POST-hulp

// Body als JSON-object, of NULL + 400 (web_server.py: _read_json_body()).
static cJSON * read_json_body(void)
{
    const String & body = s_srv->arg("plain");
    if(body.length() == 0 || body.length() > MAX_REQUEST_BODY_BYTES) {
        send_result(400, false, "ontbrekende of te grote request body");
        return nullptr;
    }
    cJSON * data = cJSON_Parse(body.c_str());
    if(!cJSON_IsObject(data)) {
        cJSON_Delete(data);
        send_result(400, false, "verwacht een JSON-object");
        return nullptr;
    }
    return data;
}

// Python: str(body[k]) - wat de writer-scripts als argv kregen.
static void arg_str(const cJSON * v, char * out, size_t out_size)
{
    if(cJSON_IsString(v)) snprintf(out, out_size, "%s", v->valuestring);
    else if(cJSON_IsNumber(v)) snprintf(out, out_size, "%.17g", v->valuedouble);
    else if(cJSON_IsBool(v)) snprintf(out, out_size, "%s", cJSON_IsTrue(v) ? "True" : "False");
    else snprintf(out, out_size, "%s", "None");
}

// Python: truthiness van body["..."] (bool, getal != 0, niet-lege string).
static bool truthy(const cJSON * v)
{
    if(cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if(cJSON_IsNumber(v)) return v->valuedouble != 0;
    if(cJSON_IsString(v)) return v->valuestring[0] != '\0';
    return false;
}

// Python: float(s) - hele string, spaties eromheen toegestaan, eindig getal.
static bool parse_float(const char * s, double * out)
{
    char * end;
    *out = strtod(s, &end);
    if(end == s) return false;
    while(*end == ' ' || *end == '\t') end++;
    return *end == '\0' && isfinite(*out);
}

// Missende velden -> 400 met dezelfde tekst als web_server.py.
static bool require_fields(const cJSON * body, const char * const * keys, int n)
{
    String missing;
    for(int i = 0; i < n; i++) {
        if(!cJSON_HasObjectItem(body, keys[i])) {
            if(missing.length()) missing += ", ";
            missing += keys[i];
        }
    }
    if(missing.length() == 0) return true;
    send_result(400, false, (String("ontbrekende velden: ") + missing).c_str());
    return false;
}

// web_server.py: _finish_settings_write() - op de ESP32 leest de daemon de
// instellingen bij de volgende cyclus opnieuw in (Pi: daemon-herstart).
static void finish_settings_write(bool ok, const char * error)
{
    if(!ok) {
        send_result(200, false, error);
        return;
    }
    daemon_reload_config();
    send_result(200, true, nullptr);
}

// ---------------------------------------------------------------- POST

static void post_solar(void)
{
    cJSON * body = read_json_body();
    if(!body) return;
    char error[160] = "";
    bool ok;
    if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "no_solar_panels"))) {
        ok = solar_settings_write_no_panels(error, sizeof(error));
    } else {
        static const char * const required[] = {"postcode", "huisnummer", "panel_count", "panel_wp",
                                                "richting", "forecast_factor", "declination", "azimuth"};
        if(!require_fields(body, required, 8)) {
            cJSON_Delete(body);
            return;
        }
        char a[8][64];
        for(int i = 0; i < 8; i++) arg_str(cJSON_GetObjectItemCaseSensitive(body, required[i]), a[i], sizeof(a[i]));
        ok = solar_settings_write(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], error, sizeof(error));
    }
    cJSON_Delete(body);
    finish_settings_write(ok, error);
}

// Port van dashboard-web/battery_settings_writer.py (zelfde validatie en foutteksten).
static bool write_battery_settings(const cJSON * body, char * error, size_t error_size)
{
    char capacity_str[64], max_power_str[64], min_soc_str[64], max_soc_str[64], loss_str[64];
    arg_str(cJSON_GetObjectItemCaseSensitive(body, "capacity_kwh"), capacity_str, sizeof(capacity_str));
    arg_str(cJSON_GetObjectItemCaseSensitive(body, "max_power_w"), max_power_str, sizeof(max_power_str));
    arg_str(cJSON_GetObjectItemCaseSensitive(body, "min_soc_pct"), min_soc_str, sizeof(min_soc_str));
    arg_str(cJSON_GetObjectItemCaseSensitive(body, "max_soc_pct"), max_soc_str, sizeof(max_soc_str));
    arg_str(cJSON_GetObjectItemCaseSensitive(body, "energy_loss_pct"), loss_str, sizeof(loss_str));
    bool active = truthy(cJSON_GetObjectItemCaseSensitive(body, "active_control_enabled"));

    double capacity, max_power, min_soc, max_soc, loss;
    if(!parse_float(capacity_str, &capacity) || capacity <= 0) {
        snprintf(error, error_size, "ongeldige capaciteit: %s", capacity_str);
        return false;
    }
    if(!parse_float(max_power_str, &max_power) || max_power <= 0) {
        snprintf(error, error_size, "ongeldig max vermogen: %s", max_power_str);
        return false;
    }
    if(!parse_float(min_soc_str, &min_soc) || !(0 <= min_soc && min_soc < 100)) {
        snprintf(error, error_size, "ongeldige min_soc_pct (0-100): %s", min_soc_str);
        return false;
    }
    if(!parse_float(max_soc_str, &max_soc) || !(0 < max_soc && max_soc <= 100)) {
        snprintf(error, error_size, "ongeldige max_soc_pct (0-100): %s", max_soc_str);
        return false;
    }
    if(min_soc >= max_soc) {
        snprintf(error, error_size, "min_soc_pct moet lager zijn dan max_soc_pct");
        return false;
    }
    if(!parse_float(loss_str, &loss) || !(0 <= loss && loss < 100)) {
        snprintf(error, error_size, "ongeldige energy_loss_pct (0-100): %s", loss_str);
        return false;
    }

    cJSON * config = cJSON_CreateObject();
    cJSON_AddNumberToObject(config, "capacity_kwh", capacity);
    cJSON_AddNumberToObject(config, "max_power_w", max_power);
    cJSON_AddBoolToObject(config, "active_control_enabled", active);
    cJSON_AddNumberToObject(config, "min_soc_pct", min_soc);
    cJSON_AddNumberToObject(config, "max_soc_pct", max_soc);
    cJSON_AddNumberToObject(config, "energy_loss_pct", loss);
    bool ok = config_write_json(BMS_BATTERY_CONFIG_PATH, config);
    cJSON_Delete(config);
    if(!ok) snprintf(error, error_size, "kon %s niet wegschrijven", BMS_BATTERY_CONFIG_PATH);
    return ok;
}

static void post_battery(void)
{
    cJSON * body = read_json_body();
    if(!body) return;
    static const char * const required[] = {"capacity_kwh", "max_power_w", "active_control_enabled",
                                            "min_soc_pct", "max_soc_pct", "energy_loss_pct"};
    if(!require_fields(body, required, 6)) {
        cJSON_Delete(body);
        return;
    }
    char error[160] = "";
    bool ok = write_battery_settings(body, error, sizeof(error));
    cJSON_Delete(body);
    if(ok) BMS_LOGI(TAG, "batterij-instellingen opgeslagen via de website");
    finish_settings_write(ok, error);
}

// Port van dashboard-web/gas_settings_writer.py.
static void post_gas(void)
{
    cJSON * body = read_json_body();
    if(!body) return;
    if(!cJSON_HasObjectItem(body, "enabled") || !cJSON_HasObjectItem(body, "unit")) {
        cJSON_Delete(body);
        send_result(400, false, "ontbrekende velden: enabled, unit");
        return;
    }
    bool enabled = truthy(cJSON_GetObjectItemCaseSensitive(body, "enabled"));
    char unit[32];
    arg_str(cJSON_GetObjectItemCaseSensitive(body, "unit"), unit, sizeof(unit));
    cJSON_Delete(body);

    char error[160] = "";
    bool ok = false;
    if(strcmp(unit, "kwh") != 0 && strcmp(unit, "m3") != 0) {
        snprintf(error, sizeof(error), "ongeldige unit (kwh of m3 verwacht): %s", unit);
    } else {
        cJSON * config = cJSON_CreateObject();
        cJSON_AddBoolToObject(config, "enabled", enabled);
        cJSON_AddStringToObject(config, "unit", unit);
        ok = config_write_json(BMS_GAS_CONFIG_PATH, config);
        cJSON_Delete(config);
        if(!ok) snprintf(error, sizeof(error), "kon %s niet wegschrijven", BMS_GAS_CONFIG_PATH);
    }
    finish_settings_write(ok, error);
}

static void post_savings_reset(void)
{
    char error[160] = "";
    bool ok = storage_reset_savings(error, sizeof(error));
    finish_settings_write(ok, error);
}

// ---------------------------------------------------------------- registratie

void web_dashboard_register(WebServer & server)
{
    s_srv = &server;
    server.on("/api/battery", HTTP_GET, handle_battery);
    server.on("/api/settings", HTTP_GET, handle_settings);
    server.on("/api/settings/solar", HTTP_POST, post_solar);
    server.on("/api/settings/battery", HTTP_POST, post_battery);
    server.on("/api/settings/gas", HTTP_POST, post_gas);
    server.on("/api/settings/savings/reset", HTTP_POST, post_savings_reset);
    for(const web_asset_t & a : WEB_ASSETS) {
        const web_asset_t * asset = &a;
        server.on(a.path, HTTP_GET, [asset]() { send_asset(asset); });
        if(strcmp(a.path, "/index.html") == 0) {
            server.on("/", HTTP_GET, [asset]() { send_asset(asset); });
        }
    }
}
