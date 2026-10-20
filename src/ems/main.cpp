// EMSmachine op CrowPanel Advance 4.3" V1.0 - fase 2: netwerk, tijd, config, HTTP-API.
// Tijdelijk statusscherm; het echte dashboard komt in fase 7.
#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <sys/stat.h>
#include <time.h>

#include "board/board.h"
#include "config/config.h"
#include "core/log.h"
#include "core/paths.h"
#include "net/net.h"
#include "web/http_api.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Maak src/ems/secrets.h aan op basis van src/ems/secrets.example.h"
#endif

static const char * TAG = "main";

void probe_start(void); // probe.cpp - tijdelijk, fase 3

static lv_obj_t * s_status_label;

static void status_timer_cb(lv_timer_t * t)
{
    (void) t;
    char time_buf[32] = "nog niet gesynchroniseerd";
    if(net_time_valid()) {
        time_t now = time(nullptr);
        struct tm local;
        localtime_r(&now, &local);
        strftime(time_buf, sizeof(time_buf), "%a %d-%m-%Y %H:%M:%S", &local);
    }
    lv_label_set_text_fmt(s_status_label,
                          "WiFi: %s  (%s, %d dBm)\n"
                          "Tijd: %s\n"
                          "Intern RAM vrij: %u KB   PSRAM vrij: %u KB\n"
                          "LittleFS: %u / %u KB",
                          net_connected() ? "verbonden" : "niet verbonden", WiFi.localIP().toString().c_str(),
                          net_connected() ? WiFi.RSSI() : 0, time_buf,
                          (unsigned) (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                          (unsigned) (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                          (unsigned) (LittleFS.usedBytes() / 1024), (unsigned) (LittleFS.totalBytes() / 1024));
}

static void build_status_screen(void)
{
    lv_obj_t * scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    lv_obj_t * title = lv_label_create(scr);
    lv_label_set_text(title, "EMSmachine - fase 2: netwerk & config");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    s_status_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_status_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_22, 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, 30, 90);

    battery_config_t bat = config_read_battery(EMS_BATTERY_CONFIG_PATH);
    gas_config_t gas = config_read_gas(EMS_GAS_CONFIG_PATH);
    char marstek_host[40];
    config_read_marstek_host(EMS_MARSTEK_CONFIG_PATH, marstek_host, sizeof(marstek_host));
    cJSON * solar = config_read_solar(EMS_SOLAR_CONFIG_PATH);

    lv_obj_t * cfg = lv_label_create(scr);
    lv_obj_set_style_text_color(cfg, lv_color_make(255, 200, 0), 0);
    lv_obj_set_style_text_font(cfg, &lv_font_montserrat_20, 0);
    lv_label_set_text_fmt(cfg,
                          "Config (defaults zolang er geen bestanden zijn):\n"
                          "Batterij: %.2f kWh, %.0f W, SOC %.0f-%.0f%%, verlies %.1f%%\n"
                          "Gas: %s, eenheid %s   Marstek-IP: %s\n"
                          "Zon-config: %s",
                          bat.capacity_kwh, bat.max_power_w, bat.min_soc_pct, bat.max_soc_pct, bat.energy_loss_pct,
                          gas.enabled ? "aan" : "uit", gas.unit_is_m3 ? "m3" : "kWh", marstek_host,
                          solar ? "aanwezig" : "nog niet ingesteld");
    cJSON_Delete(solar);
    lv_obj_align(cfg, LV_ALIGN_BOTTOM_LEFT, 30, -30);

    lv_timer_create(status_timer_cb, 1000, nullptr);
    status_timer_cb(nullptr);
}

void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println("\nEMSmachine ESP32 - fase 2");

    if(!LittleFS.begin(true, EMS_FS_MOUNT)) {
        EMS_LOGE(TAG, "LittleFS mount mislukt");
    }
    mkdir(EMS_DIR, 0755);

    board_init();
    board_lvgl_lock();
    build_status_screen();
    board_lvgl_unlock();

    net_start(WIFI_SSID, WIFI_PASSWORD, OTA_PASSWORD);
    http_api_start(OTA_PASSWORD);
    probe_start();
    EMS_LOGI(TAG, "setup klaar");
}

void loop()
{
    vTaskDelete(nullptr);
}
