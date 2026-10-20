// TIJDELIJK (fase 3): leest alle databronnen en publiceert de ruwe resultaten
// op /api/state, om ze naast de Pi's /api/state te leggen. Wordt in fase 6
// vervangen door de echte daemon-loop (port van dashboard_daemon.py).
#include <Arduino.h>
#include <cJSON.h>
#include <esp_random.h>

#include "clients/marstek_client.h"
#include "clients/p1_client.h"
#include "clients/price_client.h"
#include "clients/solar_forecast_client.h"
#include "config/config.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/state_bus.h"
#include "core/timeutil.h"
#include "net/net.h"

static const char * TAG = "probe";

// Pi pollt elke 10 s; de ESP32 bewust elke 30 s + jitter (zie plan/memory).
static const uint32_t POLL_INTERVAL_MS = 30000;

static void add_curve(cJSON * obj, const char * key, const double * v, int n, bool ok)
{
    if(!ok) {
        cJSON_AddNullToObject(obj, key);
        return;
    }
    cJSON_AddItemToObject(obj, key, cJSON_CreateDoubleArray(v, n));
}

static void probe_task(void * arg)
{
    (void) arg;
    net_wait_for_time(60000);

    // Eenmalig per opstart: prijzen + zon (langzame bronnen).
    ems_date_t today = date_today();
    double price_today[24], price_tomorrow[24], gas[24];
    bool ok_today = price_fetch_price_curve(today, price_today);
    bool ok_tomorrow = price_fetch_price_curve(date_add_days(today, 1), price_tomorrow);
    bool ok_gas = price_fetch_gas_price_curve(today, gas);
    static quarter_price_curve_t quarters;
    bool ok_quarters = price_fetch_quarter_price_curve(DYNAMISCH_TARIEF_DEFAULT_HOURS_AHEAD, &quarters);

    static solar_row_t rows[SOLAR_MAX_ROWS];
    int n_rows = 0;
    char reason[96] = "";
    bool ok_solar = false;
    cJSON * solar_cfg = config_read_solar(EMS_SOLAR_CONFIG_PATH);
    if(solar_cfg) {
        double lat = cJSON_GetNumberValue(cJSON_GetObjectItem(solar_cfg, "lat"));
        double lon = cJSON_GetNumberValue(cJSON_GetObjectItem(solar_cfg, "lon"));
        double dec = cJSON_GetNumberValue(cJSON_GetObjectItem(solar_cfg, "declination"));
        double az = cJSON_GetNumberValue(cJSON_GetObjectItem(solar_cfg, "azimuth"));
        double kwp = cJSON_GetNumberValue(cJSON_GetObjectItem(solar_cfg, "kwp"));
        ok_solar = solar_fetch_forecast(lat, lon, dec, az, kwp, rows, SOLAR_MAX_ROWS, &n_rows, reason, sizeof(reason));
        cJSON_Delete(solar_cfg);
    } else {
        snprintf(reason, sizeof(reason), "geen solar_config.json");
    }
    EMS_LOGI(TAG, "prijzen vandaag=%d morgen=%d gas=%d kwartier=%d, zon=%d (%d rijen) %s", ok_today, ok_tomorrow,
             ok_gas, ok_quarters, ok_solar, n_rows, reason);

    char marstek_host[40];
    config_read_marstek_host(EMS_MARSTEK_CONFIG_PATH, marstek_host, sizeof(marstek_host));

    for(;;) {
        uint32_t t0 = millis();
        marstek_telemetry_t tel = marstek_read_telemetry(marstek_host);
        marstek_mode_t mode = marstek_get_mode(marstek_host);
        double p1_w = 0;
        bool p1_ok = p1_read_grid_power_w(&p1_w);
        uint32_t dt = millis() - t0;

        cJSON * m = cJSON_CreateObject();
        char ts[40];
        iso_utc_now(ts, sizeof(ts));
        cJSON_AddStringToObject(m, "timestamp", ts);
        if(tel.battery_power_w_valid) cJSON_AddNumberToObject(m, "battery_power_w", tel.battery_power_w);
        else cJSON_AddNullToObject(m, "battery_power_w");
        if(tel.battery_state) cJSON_AddStringToObject(m, "battery_state", tel.battery_state);
        else cJSON_AddNullToObject(m, "battery_state");
        if(tel.battery_soc_pct_valid) cJSON_AddNumberToObject(m, "battery_soc_pct", tel.battery_soc_pct);
        else cJSON_AddNullToObject(m, "battery_soc_pct");
        if(tel.solar_power_w_valid) cJSON_AddNumberToObject(m, "solar_power_w", tel.solar_power_w);
        else cJSON_AddNullToObject(m, "solar_power_w");
        // grid: P1 bij voorkeur, anders Marstek's ongrid_power (read_grid_power_w())
        if(p1_ok) cJSON_AddNumberToObject(m, "grid_power_w", p1_w);
        else if(tel.grid_power_w_valid) cJSON_AddNumberToObject(m, "grid_power_w", tel.grid_power_w);
        else cJSON_AddNullToObject(m, "grid_power_w");

        cJSON * probe = cJSON_AddObjectToObject(m, "probe");
        cJSON_AddStringToObject(probe, "marstek_host", marstek_host);
        cJSON_AddStringToObject(probe, "marstek_mode", mode.valid ? mode.mode : "(geen antwoord)");
        cJSON_AddBoolToObject(probe, "p1_ok", p1_ok);
        cJSON_AddStringToObject(probe, "p1_ip", p1_current_ip());
        cJSON_AddNumberToObject(probe, "read_ms", dt);
        add_curve(probe, "price_curve_epex", price_today, 24, ok_today);
        add_curve(probe, "price_curve_epex_tomorrow", price_tomorrow, 24, ok_tomorrow);
        add_curve(probe, "gas_price_curve_m3", gas, 24, ok_gas);
        cJSON * q = cJSON_AddObjectToObject(probe, "quarter_prices");
        if(ok_quarters) {
            for(int i = 0; i < QUARTER_PRICE_MAX; i++) {
                if(quarters.valid[i]) {
                    char k[8];
                    snprintf(k, sizeof(k), "%d", i);
                    cJSON_AddNumberToObject(q, k, quarters.value[i]);
                }
            }
        }
        cJSON * s = cJSON_AddArrayToObject(probe, "solar_rows");
        for(int i = 0; i < n_rows; i++) {
            cJSON * r = cJSON_CreateObject();
            cJSON_AddStringToObject(r, "date", rows[i].date);
            cJSON_AddNumberToObject(r, "hour", rows[i].hour);
            cJSON_AddNumberToObject(r, "watts", rows[i].watts);
            cJSON_AddItemToArray(s, r);
        }
        cJSON * stats = marstek_get_call_stats();
        if(stats) cJSON_AddItemToObject(probe, "udp_call_stats", stats);

        char * text = cJSON_PrintUnformatted(m);
        if(text) {
            state_bus_publish(text);
            free(text);
        }
        cJSON_Delete(m);

        EMS_LOGI(TAG, "batterij %s W, SOC %s, net %s W (%s), mode %s, %u ms",
                 tel.battery_power_w_valid ? String(tel.battery_power_w, 0).c_str() : "-",
                 tel.battery_soc_pct_valid ? String(tel.battery_soc_pct, 0).c_str() : "-",
                 p1_ok ? String(p1_w, 0).c_str() : "-", p1_ok ? "P1" : "Marstek", mode.valid ? mode.mode : "-",
                 (unsigned) dt);

        // 30 s + willekeurige 0,2-2,5 s jitter (zelfde idee als JITTER_CAP_S).
        uint32_t jitter = 200 + esp_random() % 2300;
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS + jitter));
    }
}

void probe_start(void)
{
    xTaskCreatePinnedToCore(probe_task, "probe", 12288, nullptr, 1, nullptr, 0);
}
