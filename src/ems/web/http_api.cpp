#include "http_api.h"

#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <time.h>

#include "../board/board.h"
#include "../config/config.h"
#include "../core/log.h"
#include "../core/state_bus.h"
#include "../net/net.h"

static const char * TAG = "http_api";

static WebServer s_server(80);
static char s_write_token[64];

// Alleen deze bestanden mogen via /api/config gezet worden (geen vrije paden).
static const char * CONFIG_WHITELIST[] = {"battery_config.json", "gas_config.json", "solar_config.json",
                                          "p1_source.json", "marstek.json"};

static bool authorized(void)
{
    if(s_write_token[0] == '\0' || s_server.header("X-EMS-Token") != s_write_token) {
        s_server.send(401, "text/plain", "missing/invalid X-EMS-Token");
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
        // Zelfde idee als web_server.py: nog geen bericht = nog geen data.
        s_server.send(503, "application/json", "{\"error\": \"no data yet\"}");
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

// Schermafdruk van het actieve LVGL-scherm als 24-bit BMP - zodat de layout
// van de geporte schermen op afstand te controleren is (fase 7).
static void handle_screenshot(void)
{
    board_lvgl_lock();
    lv_draw_buf_t * snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
    board_lvgl_unlock();
    if(!snap) {
        s_server.send(500, "text/plain", "snapshot failed (geheugen?)");
        return;
    }

    const uint32_t w = snap->header.w;
    const uint32_t h = snap->header.h;
    const uint32_t stride = snap->header.stride;
    const uint32_t row_bytes = w * 3;
    const uint32_t row_padded = (row_bytes + 3) & ~3u;
    const uint32_t data_size = row_padded * h;

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

    // LVGL's RGB888 ligt in geheugen als B,G,R - precies BMP's volgorde.
    static uint8_t pad[3] = {0};
    for(int32_t y = h - 1; y >= 0; y--) {
        s_server.sendContent((const char *) snap->data + y * stride, row_bytes);
        if(row_padded != row_bytes) {
            s_server.sendContent((const char *) pad, row_padded - row_bytes);
        }
    }

    board_lvgl_lock();
    lv_draw_buf_destroy(snap);
    board_lvgl_unlock();
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
    EMS_LOGI(TAG, "config %s %s", name.c_str(), ok ? "geschreven" : "NIET geschreven");
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
    const char * headers[] = {"X-EMS-Token"};
    s_server.collectHeaders(headers, 1);

    s_server.on("/api/health", HTTP_GET, handle_health);
    s_server.on("/api/config", HTTP_GET, handle_config);
    s_server.on("/api/config", HTTP_PUT, handle_config);
    s_server.on("/api/state", HTTP_GET, handle_state);
    s_server.on("/api/files", HTTP_GET, handle_files);
    s_server.on("/api/screenshot", HTTP_GET, handle_screenshot);
    s_server.onNotFound([]() { s_server.send(404, "text/plain", "not found"); });
    s_server.begin();
    xTaskCreatePinnedToCore(http_task, "http", 8192, nullptr, 1, nullptr, 0);
    EMS_LOGI(TAG, "HTTP-API gestart op poort 80");
}
