// BMSmachine op CrowPanel Advance 4.3" V1.0 - zelfstandige port van
// github.com/Banthum/EMS (daar nog "EMSmachine"). Stuurt de batterij aan in een
// LIVE-build, anders schaduw-modus (zie daemon/daemon.h).
#include <Arduino.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>
#include <stdio.h>
#include <sys/stat.h>

#include "board/board.h"
#include "core/log.h"
#include "core/paths.h"
#include "daemon/daemon.h"
#include "net/net.h"
#include "storage/storage.h"
#include "web/http_api.h"

extern "C" {
#include "ui/dashboard_ui.h"
#include "ui/ipc_client.h"
#include "ui/screen_common.h"
}

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Maak src/bms/secrets.h aan op basis van src/bms/secrets.example.h"
#endif

static const char * TAG = "main";

// cJSON maakt per getal een klein object; een volledig dashboardbericht heeft
// er >2000. Kleine allocaties belanden standaard in het schaarse interne RAM
// (dat WiFi/TLS/LVGL nodig hebben) - daarom alles van cJSON naar PSRAM.
static void * cjson_psram_malloc(size_t n)
{
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// mbedTLS (HTTPS naar prijs-/zon-API's) vraagt per verbinding ~30-40 KB; in
// deze core standaard uit intern RAM. Met daemon, LVGL en WiFi samen was dat
// er niet meer (ESP_ERR_HTTP_CONNECT bij elke HTTPS-call) - dus naar PSRAM.
static void * mbedtls_psram_calloc(size_t n, size_t size)
{
    return heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// dashboard-ui/src/main.c op de Pi, minus fbdev/evdev (dat doet board_init()).
static void start_dashboard_ui(void)
{
    // Groen i.p.v. LVGL's standaard blauw/rood voor primary/secondary, zodat
    // dropdown-selecties en toetsenborden bij de rest van de UI passen
    // (zelfde aanroep als in main.c op de Pi).
    lv_theme_default_init(board_display(), bgr_fix(lv_color_make(0, 150, 0)), bgr_fix(lv_color_make(0, 150, 0)),
                          false, LV_FONT_DEFAULT);
    dashboard_ui_init();
}

void setup()
{
    cJSON_Hooks hooks = {cjson_psram_malloc, heap_caps_free};
    cJSON_InitHooks(&hooks);
    mbedtls_platform_set_calloc_free(mbedtls_psram_calloc, heap_caps_free);

    Serial.begin(115200);
    delay(200);
    Serial.println("\nBMSmachine ESP32");

    if(!LittleFS.begin(true, BMS_FS_MOUNT)) {
        BMS_LOGE(TAG, "LittleFS mount mislukt");
    }
    mkdir(BMS_DIR, 0755);
    // Hernoemd van EMSmachine naar BMSmachine: bestaande database meenemen.
    struct stat st;
    if(stat(BMS_DB_PATH, &st) != 0 && stat(BMS_DB_LEGACY_PATH, &st) == 0) {
        bool ok = rename(BMS_DB_LEGACY_PATH, BMS_DB_PATH) == 0;
        BMS_LOGI(TAG, "database %s -> %s: %s", BMS_DB_LEGACY_PATH, BMS_DB_PATH, ok ? "hernoemd" : "MISLUKT");
    }
    // Een achtergebleven journal van de oude naam hoort bij geen database meer.
    if(stat(BMS_DB_LEGACY_PATH, &st) != 0) remove(BMS_DB_LEGACY_PATH "-journal");
    storage_init(BMS_DB_PATH);

    // WiFi eerst: de WiFi-driver heeft intern RAM nodig en moet dat krijgen
    // vóórdat de UI z'n buffers/taken reserveert.
    net_start(WIFI_SSID, WIFI_PASSWORD, OTA_PASSWORD);

    ipc_client_init();
    board_init();
    board_lvgl_lock();
    start_dashboard_ui();
    board_lvgl_unlock();

    http_api_start(OTA_PASSWORD);
    daemon_start();
    ipc_client_start();
    BMS_LOGI(TAG, "setup klaar");
}

void loop()
{
    vTaskDelete(nullptr);
}
