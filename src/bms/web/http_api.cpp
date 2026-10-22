#include "http_api.h"

#include <LittleFS.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <time.h>

#include "../board/board.h"

extern "C" {
#include "../ui/dashboard_ui.h"
}
#include "../config/config.h"
#include "../daemon/daemon.h"
#include "../core/log.h"
#include "../core/state_bus.h"
#include "../net/net.h"
#include "../logic/battery_scheduler.h"
#include "../storage/storage.h"
#include "web_dashboard.h"

static const char * TAG = "http_api";

static WebServer s_server(80);
static char s_write_token[64];

// Alleen deze bestanden mogen via /api/config gezet worden (geen vrije paden).
static const char * CONFIG_WHITELIST[] = {"battery_config.json", "gas_config.json", "solar_config.json",
                                          "p1_source.json", "marstek.json"};

static bool authorized(void)
{
    if(s_write_token[0] == '\0' || s_server.header("X-BMS-Token") != s_write_token) {
        s_server.send(401, "text/plain", "missing/invalid X-BMS-Token");
        return false;
    }
    return true;
}

static void send_json(int code, cJSON * json)
{
    char * text = cJSON_PrintUnformatted(json);
    s_server.sendHeader("Cache-Control", "no-store");
    s_server.send(code, "application/json", text ? text : "{}");
    free(text);
}

static void handle_health(void)
{
    cJSON * j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "uptime_s", millis() / 1000);
    cJSON_AddNumberToObject(j, "heap_internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(j, "heap_internal_largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(j, "heap_psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(j, "wifi_rssi", WiFi.RSSI());
    cJSON_AddStringToObject(j, "wifi_bssid", WiFi.BSSIDstr().c_str());
    cJSON_AddNumberToObject(j, "wifi_channel", WiFi.channel());
    cJSON_AddStringToObject(j, "ip", WiFi.localIP().toString().c_str());
    cJSON_AddBoolToObject(j, "time_valid", net_time_valid());
    char ts[32];
    time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S%z", &local);
    cJSON_AddStringToObject(j, "local_time", ts);
    cJSON_AddNumberToObject(j, "fs_used", LittleFS.usedBytes());
    cJSON_AddNumberToObject(j, "fs_total", LittleFS.totalBytes());
    cJSON_AddNumberToObject(j, "state_version", state_bus_version());
    send_json(200, j);
    cJSON_Delete(j);
}

static void handle_state(void)
{
    char * json = state_bus_copy(nullptr);
    s_server.sendHeader("Cache-Control", "no-store");
    if(!json) {
        // Zelfde als web_server.py: nog geen bericht = null (de website wacht dan).
        s_server.send(200, "application/json", "null");
        return;
    }
    s_server.send(200, "application/json", json);
    free(json);
}

static void handle_files(void)
{
    cJSON * arr = cJSON_CreateArray();
    File dir = LittleFS.open("/ems");
    if(dir && dir.isDirectory()) {
        for(File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            cJSON * e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "name", f.name());
            cJSON_AddNumberToObject(e, "size", f.size());
            cJSON_AddItemToArray(arr, e);
        }
    }
    send_json(200, arr);
    cJSON_Delete(arr);
}

// Schermafdruk als 24-bit BMP, rechtstreeks uit de framebuffer van het paneel
// (dus precies wat er op het scherm staat) - zodat de layout van de geporte
// schermen op afstand te controleren is (fase 7). Geen lv_snapshot meer: die
// tekende het scherm opnieuw in een buffer van 1,15 MB PSRAM en de WiFi-
// zender liep dan vast op PSRAM-verkeer (task-watchdog). Elke regel wordt
// eerst naar een kleine buffer in intern RAM omgezet.
static void handle_screenshot(void)
{
    const uint16_t * fb = board_framebuffer();
    if(!fb) {
        s_server.send(500, "text/plain", "geen framebuffer");
        return;
    }

    const uint32_t w = BOARD_LCD_H_RES;
    const uint32_t h = BOARD_LCD_V_RES;
    const uint32_t row_bytes = w * 3;  // 2400, al een veelvoud van 4
    const uint32_t data_size = row_bytes * h;

    uint8_t hdr[54] = {0};
    hdr[0] = 'B';
    hdr[1] = 'M';
    uint32_t file_size = 54 + data_size;
    memcpy(&hdr[2], &file_size, 4);
    uint32_t offset = 54;
    memcpy(&hdr[10], &offset, 4);
    uint32_t info_size = 40;
    memcpy(&hdr[14], &info_size, 4);
    int32_t bw = w, bh = h; // positief = bottom-up
    memcpy(&hdr[18], &bw, 4);
    memcpy(&hdr[22], &bh, 4);
    uint16_t planes = 1, bpp = 24;
    memcpy(&hdr[26], &planes, 2);
    memcpy(&hdr[28], &bpp, 2);
    memcpy(&hdr[34], &data_size, 4);

    s_server.setContentLength(file_size);
    s_server.sendHeader("Cache-Control", "no-store");
    s_server.send(200, "image/bmp", "");
    s_server.sendContent((const char *) hdr, sizeof(hdr));

    static uint8_t row[BOARD_LCD_H_RES * 3];
    for(int32_t y = h - 1; y >= 0; y--) {
        const uint16_t * src = fb + y * w;
        for(uint32_t x = 0; x < w; x++) {
            // LovyanGFX bewaart RGB565 byte-gewisseld (big-endian) in de framebuffer.
            uint16_t c = (uint16_t) ((src[x] >> 8) | (src[x] << 8));
            uint8_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
            row[x * 3 + 0] = (b << 3) | (b >> 2);
            row[x * 3 + 1] = (g << 2) | (g >> 4);
            row[x * 3 + 2] = (r << 3) | (r >> 2);
        }
        s_server.sendContent((const char *) row, row_bytes);
    }
}

// GET /api/config?name=x.json -> inhoud; PUT /api/config?name=x.json (body =
// JSON, met token) -> valideren en atomair wegschrijven naar /littlefs/ems/.
static void handle_config(void)
{
    String name = s_server.arg("name");
    bool allowed = false;
    for(const char * w : CONFIG_WHITELIST) {
        if(name == w) {
            allowed = true;
        }
    }
    if(!allowed) {
        s_server.send(400, "text/plain", "unknown config name");
        return;
    }
    String path = String("/littlefs/ems/") + name;

    if(s_server.method() == HTTP_GET) {
        cJSON * json = config_load_json(path.c_str());
        if(!json) {
            s_server.send(404, "text/plain", "not set");
            return;
        }
        send_json(200, json);
        cJSON_Delete(json);
        return;
    }

    if(!authorized()) {
        return;
    }
    cJSON * json = cJSON_Parse(s_server.arg("plain").c_str());
    if(!cJSON_IsObject(json)) {
        cJSON_Delete(json);
        s_server.send(400, "text/plain", "body is not a JSON object");
        return;
    }
    bool ok = config_write_json(path.c_str(), json);
    cJSON_Delete(json);
    s_server.send(ok ? 200 : 500, "text/plain", ok ? "ok" : "write failed");
    BMS_LOGI(TAG, "config %s %s", name.c_str(), ok ? "geschreven" : "NIET geschreven");
    if(ok) {
        // Zoals de Pi z'n daemon herstart na het opslaan van instellingen.
        daemon_reload_config();
    }
}

// ---- upload (multipart) van import-bestanden naar /littlefs/ems/ ----
static const char * UPLOAD_WHITELIST[] = {"import.db", "default_consumption_profile.csv"};
static FILE * s_upload_file = nullptr;
static bool s_upload_ok = false;
static String s_upload_name;

static void handle_upload_chunk(void)
{
    HTTPUpload & up = s_server.upload();
    if(up.status == UPLOAD_FILE_START) {
        s_upload_ok = false;
        s_upload_name = s_server.arg("name");
        bool allowed = false;
        for(const char * w : UPLOAD_WHITELIST) {
            if(s_upload_name == w) allowed = true;
        }
        bool token_ok = s_write_token[0] != '\0' && s_server.header("X-BMS-Token") == s_write_token;
        if(allowed && token_ok) {
            String path = String("/littlefs/ems/") + s_upload_name + ".part";
            s_upload_file = fopen(path.c_str(), "wb");
            s_upload_ok = s_upload_file != nullptr;
        }
    } else if(up.status == UPLOAD_FILE_WRITE) {
        if(s_upload_file && fwrite(up.buf, 1, up.currentSize, s_upload_file) != up.currentSize) {
            s_upload_ok = false;
        }
    } else if(up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
        if(s_upload_file) {
            fclose(s_upload_file);
            s_upload_file = nullptr;
        }
        String part = String("/littlefs/ems/") + s_upload_name + ".part";
        String final_path = String("/littlefs/ems/") + s_upload_name;
        if(up.status == UPLOAD_FILE_END && s_upload_ok) {
            remove(final_path.c_str());
            s_upload_ok = rename(part.c_str(), final_path.c_str()) == 0;
        } else {
            remove(part.c_str());
            s_upload_ok = false;
        }
    }
}

static void handle_upload_done(void)
{
    if(!s_upload_ok) {
        s_server.send(400, "text/plain", "upload geweigerd of mislukt (token/naam?)");
        return;
    }
    s_server.send(200, "text/plain", "ok");
    BMS_LOGI(TAG, "upload %s ontvangen", s_upload_name.c_str());
}

// ---- firmware-update via HTTP (multipart, met token) ----
// Alternatief voor ArduinoOTA/espota: daarbij moet het bord een TCP-verbinding
// terug naar de pc openen, en die blokkeert de Windows-firewall op een
// "openbaar" netwerk. Hier stuurt de pc alles naar het bord:
//     curl -H "X-BMS-Token: <OTA_PASSWORD>" -F fw=@firmware.bin http://<bord>/api/ota
static bool s_ota_ok = false;
static bool s_ota_started = false;

static void handle_ota_chunk(void)
{
    HTTPUpload & up = s_server.upload();
    if(up.status == UPLOAD_FILE_START) {
        s_ota_ok = false;
        s_ota_started = false;
        bool token_ok = s_write_token[0] != '\0' && s_server.header("X-BMS-Token") == s_write_token;
        if(!token_ok) return;
        BMS_LOGI(TAG, "firmware-update via HTTP gestart");
        storage_persist(); // database staat in PSRAM - niet kwijtraken bij de herstart
        s_ota_started = Update.begin(UPDATE_SIZE_UNKNOWN);
        s_ota_ok = s_ota_started;
        if(!s_ota_started) BMS_LOGW(TAG, "Update.begin mislukt: %s", Update.errorString());
    } else if(up.status == UPLOAD_FILE_WRITE) {
        if(s_ota_ok && Update.write(up.buf, up.currentSize) != up.currentSize) {
            BMS_LOGW(TAG, "Update.write mislukt: %s", Update.errorString());
            s_ota_ok = false;
        }
    } else if(up.status == UPLOAD_FILE_END) {
        if(s_ota_ok) {
            s_ota_ok = Update.end(true);
            if(!s_ota_ok) BMS_LOGW(TAG, "Update.end mislukt: %s", Update.errorString());
        } else if(s_ota_started) {
            Update.abort();
        }
    } else if(up.status == UPLOAD_FILE_ABORTED) {
        if(s_ota_started) Update.abort();
        s_ota_ok = false;
    }
}

static void handle_ota_done(void)
{
    if(!s_ota_ok) {
        s_server.send(400, "text/plain", "firmware-update geweigerd of mislukt (token? zie /api/log)");
        return;
    }
    s_server.send(200, "text/plain", "ok, herstart");
    BMS_LOGI(TAG, "firmware-update geslaagd, herstart");
    delay(500);
    ESP.restart();
}

// POST /api/import: neemt de historie over uit een eerder geüploade import.db
// (de Pi-export) en verwijdert dat bestand daarna.
static void handle_import(void)
{
    if(!authorized()) {
        return;
    }
    const char * path = "/littlefs/ems/import.db";
    cJSON * report = cJSON_CreateObject();
    bool ok = storage_import_from_db(path, report);
    remove(path);
    cJSON_AddBoolToObject(report, "ok", ok);
    send_json(ok ? 200 : 500, report);
    cJSON_Delete(report);
}

static cJSON * opt_array(const opt_d * v, int n)
{
    cJSON * arr = cJSON_CreateArray();
    for(int i = 0; i < n; i++) {
        cJSON_AddItemToArray(arr, v[i].valid ? cJSON_CreateNumber(v[i].v) : cJSON_CreateNull());
    }
    return arr;
}

// GET /api/debug/storage: wat de opslaglaag nu berekent - om naast de Pi te leggen.
static void handle_debug_storage(void)
{
    static opt_d today_curve[QUARTERS_PER_DAY], tomorrow_curve[QUARTERS_PER_DAY], solar_q[QUARTERS_PER_DAY];
    static opt_d solar_h[24];
    ems_date_t today = date_today();
    if(s_server.hasArg("timing")) {
        // Diagnose: tijd per bouwsteen van de verbruiksvoorspelling.
        static opt_d profile[QUARTERS_PER_DAY];
        uint32_t t0 = millis();
        storage_load_default_consumption_profile("/littlefs/ems/default_consumption_profile.csv", profile);
        uint32_t t1 = millis();
        storage_expected_consumption(time(nullptr), 14, 1, profile);
        uint32_t t2 = millis();
        storage_expected_consumption(time(nullptr) + 900, 14, 1, profile);
        uint32_t t3 = millis();
        char buf[160];
        snprintf(buf, sizeof(buf), "{\"profile_ms\":%u,\"query1_ms\":%u,\"query2_ms\":%u}", (unsigned) (t1 - t0),
                 (unsigned) (t2 - t1), (unsigned) (t3 - t2));
        s_server.send(200, "application/json", buf);
        return;
    }
    storage_expected_consumption_curve(today, today_curve);
    storage_expected_consumption_curve(date_add_days(today, 1), tomorrow_curve);
    storage_solar_forecast_curve(today, solar_h);
    storage_solar_forecast_curve_quarterly(today, solar_q);
    savings_summary_t s = storage_savings_summary(today);

    cJSON * j = cJSON_CreateObject();
    cJSON_AddItemToObject(j, "expected_consumption_curve", opt_array(today_curve, QUARTERS_PER_DAY));
    cJSON_AddItemToObject(j, "expected_consumption_curve_tomorrow", opt_array(tomorrow_curve, QUARTERS_PER_DAY));
    cJSON_AddItemToObject(j, "solar_forecast_curve", opt_array(solar_h, 24));
    cJSON_AddItemToObject(j, "solar_forecast_curve_quarterly_today", opt_array(solar_q, QUARTERS_PER_DAY));
    cJSON_AddNumberToObject(j, "shadow_soc_kwh", storage_load_pool_state());
    char y[11];
    date_iso(date_add_days(today, -1), y);
    double baseline;
    if(storage_overnight_baseline_load_w(y, &baseline)) cJSON_AddNumberToObject(j, "overnight_baseline_yesterday_w", baseline);
    else cJSON_AddNullToObject(j, "overnight_baseline_yesterday_w");
    const struct {
        const char * name;
        savings_pair_t p;
    } periods[] = {{"dag", s.dag}, {"week", s.week}, {"maand", s.maand}, {"jaar", s.jaar}, {"altijd", s.altijd}};
    for(auto & p : periods) {
        char k[48];
        snprintf(k, sizeof(k), "savings_%s_batterij_eur", p.name);
        cJSON_AddNumberToObject(j, k, p.p.batterij_eur);
        snprintf(k, sizeof(k), "savings_%s_ems_eur", p.name);
        cJSON_AddNumberToObject(j, k, p.p.ems_eur);
    }
    send_json(200, j);
    cJSON_Delete(j);
}

// POST /api/debug/plan: draait plan_day() op aangeleverde invoer (zelfde vorm
// als de Pi gebruikt) - om het ESP32-plan naast dat van de Pi te leggen.
// Body: {now_epoch, soc_pct, capacity_kwh, max_power_w, min_soc_pct, max_soc_pct,
//        energy_loss_pct, price_hourly[48], quarter_prices[192|null...],
//        consumption[192|null...], solar_hourly[48|null...]}
static void json_to_opt(const cJSON * arr, opt_d * out, int max_n, int * n_out)
{
    int n = 0;
    const cJSON * v;
    cJSON_ArrayForEach(v, arr)
    {
        if(n == max_n) break;
        out[n++] = cJSON_IsNumber(v) ? opt_some(v->valuedouble) : opt_none();
    }
    *n_out = n;
}

static void handle_debug_plan(void)
{
    cJSON * body = cJSON_Parse(s_server.arg("plain").c_str());
    if(!body) {
        s_server.send(400, "text/plain", "body is geen JSON");
        return;
    }
    static opt_d price[48], cons[192], solar[48], qp[192];
    static bool q_valid[192];
    static double q_value[192];
    int n_price = 0, n_cons = 0, n_solar = 0, n_q = 0;
    json_to_opt(cJSON_GetObjectItem(body, "price_hourly"), price, 48, &n_price);
    json_to_opt(cJSON_GetObjectItem(body, "consumption"), cons, 192, &n_cons);
    json_to_opt(cJSON_GetObjectItem(body, "solar_hourly"), solar, 48, &n_solar);
    json_to_opt(cJSON_GetObjectItem(body, "quarter_prices"), qp, 192, &n_q);
    for(int i = 0; i < n_q; i++) {
        q_valid[i] = qp[i].valid;
        q_value[i] = qp[i].v;
    }
    plan_inputs_t in = {price, n_price, cons, n_cons, solar, n_solar, n_q ? q_valid : nullptr, q_value, n_q};

    static plan_result_t * plan = nullptr;
    if(!plan) plan = (plan_result_t *) heap_caps_malloc(sizeof(plan_result_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    auto num = [&](const char * k) { return cJSON_GetNumberValue(cJSON_GetObjectItem(body, k)); };
    plan_day(&in, num("soc_pct"), num("capacity_kwh"), num("max_power_w"), (time_t) num("now_epoch"), 48.0,
             num("min_soc_pct"), num("max_soc_pct"), num("energy_loss_pct"), plan);
    cJSON_Delete(body);

    static const char * actions[] = {"none", "auto", "charge", "block"};
    cJSON * j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "compute_ms", plan->compute_ms);
    cJSON_AddNumberToObject(j, "scenarios_tried", plan->scenarios_tried);
    cJSON_AddStringToObject(j, "action", actions[plan->current_decision.action]);
    cJSON * w = cJSON_AddArrayToObject(j, "windows_chosen");
    for(int i = 0; i < plan->n_windows; i++) cJSON_AddItemToArray(w, cJSON_CreateString(plan->windows_start_time[i]));
    cJSON * bw = cJSON_AddArrayToObject(j, "block_windows_chosen");
    for(int i = 0; i < plan->n_block_windows; i++)
        cJSON_AddItemToArray(bw, cJSON_CreateString(plan->block_windows_start_time[i]));
    cJSON_AddItemToObject(j, "battery_plan_eigen_zon_wh", opt_array(plan->eigen_zon_wh, QUARTERS_PER_PLAN));
    cJSON_AddItemToObject(j, "battery_plan_actief_gestuurd_wh", opt_array(plan->actief_gestuurd_wh, QUARTERS_PER_PLAN));
    cJSON_AddItemToObject(j, "battery_plan_verwacht_verbruik_wh",
                          opt_array(plan->verwacht_verbruik_wh, QUARTERS_PER_PLAN));
    cJSON_AddItemToObject(j, "battery_plan_geblokkeerd_wh", opt_array(plan->geblokkeerd_wh, QUARTERS_PER_PLAN));
    cJSON_AddItemToObject(j, "battery_soc_forecast_pct", opt_array(plan->soc_forecast_pct, QUARTERS_PER_PLAN));
    send_json(200, j);
    cJSON_Delete(j);
}

static void write_to_client(const char * text, void * ctx)
{
    (void) ctx;
    s_server.sendContent(text);
}

static void send_csv(int (*exporter)(storage_write_fn, void *), const char * filename)
{
    s_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    s_server.sendHeader("Content-Disposition", String("attachment; filename=") + filename);
    s_server.send(200, "text/csv", "");
    exporter(write_to_client, nullptr);
    s_server.sendContent("");
}

// GET /api/log: de laatste logregels (ringbuffer), voor als er geen seriele
// kabel aan het bord hangt. Eerst kopieren, zodat de log-mutex niet vastzit
// terwijl er over WiFi verstuurd wordt.
struct log_copy_t {
    char * buf;
    size_t len;
};

static void log_copy_cb(const char * data, size_t len, void * ctx)
{
    log_copy_t * c = (log_copy_t *) ctx;
    memcpy(c->buf + c->len, data, len);
    c->len += len;
}

static void handle_log(void)
{
    log_copy_t c = {(char *) heap_caps_malloc(16 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT), 0};
    if(!c.buf) {
        s_server.send(500, "text/plain", "geen geheugen");
        return;
    }
    ems_log_copy(log_copy_cb, &c);
    s_server.setContentLength(c.len);
    s_server.send(200, "text/plain; charset=utf-8", "");
    s_server.sendContent(c.buf, c.len);
    heap_caps_free(c.buf);
}

// Diagnose beeldstabiliteit: een taak (of WiFi) tijdelijk stilzetten om te zien
// welke belasting de RGB-DMA uit PSRAM laat achterlopen.
// GET /api/debug/pause?task=lvgl|daemon|ui_state|ota|wifi&s=30
struct pause_req_t {
    char task[16];
    uint32_t seconds;
};

static void pause_task(void * arg)
{
    pause_req_t * req = (pause_req_t *) arg;
    if(strcmp(req->task, "wifi") == 0) {
        WiFi.mode(WIFI_OFF);
        vTaskDelay(pdMS_TO_TICKS(req->seconds * 1000));
        WiFi.mode(WIFI_STA);
        WiFi.begin();
    } else {
        TaskHandle_t h = xTaskGetHandle(req->task);
        if(h) {
            vTaskSuspend(h);
            vTaskDelay(pdMS_TO_TICKS(req->seconds * 1000));
            vTaskResume(h);
        }
    }
    free(req);
    vTaskDelete(nullptr);
}

static void handle_pause(void)
{
    pause_req_t * req = (pause_req_t *) calloc(1, sizeof(pause_req_t));
    strlcpy(req->task, s_server.arg("task").c_str(), sizeof(req->task));
    req->seconds = (uint32_t) s_server.arg("s").toInt();
    if(req->seconds == 0 || req->seconds > 300) req->seconds = 30;
    if(strcmp(req->task, "wifi") != 0 && xTaskGetHandle(req->task) == nullptr) {
        free(req);
        s_server.send(404, "text/plain", "onbekende taak");
        return;
    }
    s_server.send(200, "text/plain", "ok");
    xTaskCreatePinnedToCore(pause_task, "pause", 3072, req, 3, nullptr, 0);
}

// Diagnose: schermvullend testpatroon (grijs raster op zwart) bovenop het
// dashboard - kleine verschuivingen van het beeld zijn daarop met een webcam
// goed meetbaar. GET /api/debug/pattern?on=1|0
static lv_obj_t * s_pattern = nullptr;

static void handle_pattern(void)
{
    bool on = s_server.arg("on") == "1";
    board_lvgl_lock();
    if(on && !s_pattern) {
        s_pattern = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_pattern);
        lv_obj_set_size(s_pattern, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
        lv_obj_set_style_bg_color(s_pattern, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_pattern, LV_OPA_COVER, 0);
        for(int x = 20; x < BOARD_LCD_H_RES; x += 40) {
            lv_obj_t * l = lv_obj_create(s_pattern);
            lv_obj_remove_style_all(l);
            lv_obj_set_pos(l, x, 0);
            lv_obj_set_size(l, 3, BOARD_LCD_V_RES);
            lv_obj_set_style_bg_color(l, lv_color_hex(0x505050), 0); // grijs: webcam niet verzadigen
            lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
        }
        for(int y = 20; y < BOARD_LCD_V_RES; y += 40) {
            lv_obj_t * l = lv_obj_create(s_pattern);
            lv_obj_remove_style_all(l);
            lv_obj_set_pos(l, 0, y);
            lv_obj_set_size(l, BOARD_LCD_H_RES, 3);
            lv_obj_set_style_bg_color(l, lv_color_hex(0x505050), 0); // grijs: webcam niet verzadigen
            lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
        }
    } else if(!on && s_pattern) {
        lv_obj_delete(s_pattern);
        s_pattern = nullptr;
    }
    board_lvgl_unlock();
    s_server.send(200, "text/plain", "ok");
}

static void http_task(void * arg)
{
    (void) arg;
    for(;;) {
        s_server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void http_api_start(const char * write_token)
{
    snprintf(s_write_token, sizeof(s_write_token), "%s", write_token ? write_token : "");
    const char * headers[] = {"X-BMS-Token"};
    s_server.collectHeaders(headers, 1);

    s_server.on("/api/health", HTTP_GET, handle_health);
    s_server.on("/api/config", HTTP_GET, handle_config);
    s_server.on("/api/config", HTTP_PUT, handle_config);
    s_server.on("/api/upload", HTTP_POST, handle_upload_done, handle_upload_chunk);
    s_server.on("/api/ota", HTTP_POST, handle_ota_done, handle_ota_chunk);
    s_server.on("/api/import", HTTP_POST, handle_import);
    s_server.on("/api/debug/storage", HTTP_GET, handle_debug_storage);
    s_server.on("/api/debug/plan", HTTP_POST, handle_debug_plan);
    // Wissel van tab (0=Energiestromen ... 5=Besparing) - voor screenshots op afstand.
    s_server.on("/api/debug/tab", HTTP_GET, []() {
        int i = s_server.arg("i").toInt();
        board_lvgl_lock();
        dashboard_ui_select_tab(i);
        board_lvgl_unlock();
        s_server.send(200, "text/plain", "ok");
    });
    // Open de Instellingen-pagina (0) of een onderdeel (1-4), -1 = dashboard - voor screenshots.
    s_server.on("/api/debug/settings", HTTP_GET, []() {
        int i = s_server.hasArg("i") ? s_server.arg("i").toInt() : 0;
        board_lvgl_lock();
        dashboard_ui_open_settings(i);
        board_lvgl_unlock();
        s_server.send(200, "text/plain", "ok");
    });
    // Vervangen /home/pi/consumption_quarterly.csv en battery_log.csv van de Pi.
    s_server.on("/export/consumption_quarterly.csv", HTTP_GET,
                []() { send_csv(storage_export_consumption_csv, "consumption_quarterly.csv"); });
    s_server.on("/export/battery_log.csv", HTTP_GET,
                []() { send_csv(storage_export_battery_log_csv, "battery_log.csv"); });
    s_server.on("/api/state", HTTP_GET, handle_state);
    s_server.on("/api/files", HTTP_GET, handle_files);
    s_server.on("/api/screenshot", HTTP_GET, handle_screenshot);
    s_server.on("/api/log", HTTP_GET, handle_log);
    s_server.on("/api/debug/pause", HTTP_GET, handle_pause);
    s_server.on("/api/debug/pattern", HTTP_GET, handle_pattern);
    // Testvenster voor de aansturing (alleen LIVE-build), met token:
    //   POST /api/control/test?action=charge&seconds=180[&power_w=-1000]   (action=stop breekt af)
    s_server.on("/api/control/test", HTTP_POST, []() {
        if(!authorized()) return;
        double power = s_server.hasArg("power_w") ? s_server.arg("power_w").toDouble() : NAN;
        double seconds = s_server.hasArg("seconds") ? s_server.arg("seconds").toDouble() : 180;
        char error[128];
        if(daemon_control_test(s_server.arg("action").c_str(), power, seconds, error, sizeof(error))) {
            s_server.send(200, "text/plain", "ok - start in de volgende cyclus (max 10 s)");
        } else {
            s_server.send(400, "text/plain", error);
        }
    });
    web_dashboard_register(s_server);
    s_server.onNotFound([]() { s_server.send(404, "text/plain", "not found"); });
    s_server.begin();
    xTaskCreatePinnedToCore(http_task, "http", 12288, nullptr, 1, nullptr, 0); // ruim: /api/ota en import schrijven de database weg (SQLite)
    BMS_LOGI(TAG, "HTTP-API gestart op poort 80");
}
