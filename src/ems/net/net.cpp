#include "net.h"

#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

#include "../core/log.h"
#include "../storage/storage.h"

static const char * TAG = "net";

// POSIX-TZ voor Europe/Amsterdam (CET/CEST, wisseling laatste zondag maart/oktober).
// Vervangt Python's zoneinfo.ZoneInfo("Europe/Amsterdam") / localtime.
static const char * TZ_AMSTERDAM = "CET-1CEST,M3.5.0,M10.5.0/3";

static volatile bool s_time_valid = false;
static bool s_services_started = false;
static char s_ota_password[64];

static void on_time_sync(struct timeval * tv)
{
    (void) tv;
    if(!s_time_valid) {
        s_time_valid = true;
        EMS_LOGI(TAG, "NTP-tijd gezet");
    }
}

static void ota_task(void * arg)
{
    (void) arg;
    for(;;) {
        ArduinoOTA.handle();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void start_services(void)
{
    if(s_services_started) {
        return;
    }
    s_services_started = true;

    if(MDNS.begin(EMS_HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
    } else {
        EMS_LOGW(TAG, "mDNS start mislukt");
    }

    ArduinoOTA.setHostname(EMS_HOSTNAME);
    ArduinoOTA.setPassword(s_ota_password);
    ArduinoOTA.onStart([]() {
        EMS_LOGI(TAG, "OTA-update gestart");
        storage_persist(); // database staat in PSRAM - niet kwijtraken bij de herstart
    });
    ArduinoOTA.onError([](ota_error_t e) { EMS_LOGW(TAG, "OTA-fout %d", (int) e); });
    ArduinoOTA.begin();
    xTaskCreatePinnedToCore(ota_task, "ota", 12288, nullptr, 1, nullptr, 0); // onStart schrijft de database weg (SQLite)
}

static void on_wifi_event(WiFiEvent_t event, WiFiEventInfo_t info)
{
    switch(event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            EMS_LOGI(TAG, "WiFi verbonden, IP %s, RSSI %d dBm, access point %s, kanaal %d",
                     WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.BSSIDstr().c_str(), WiFi.channel());
            start_services();
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            EMS_LOGW(TAG, "WiFi verbroken (reden %d)", (int) info.wifi_sta_disconnected.reason);
            break;
        default:
            break;
    }
}

void net_start(const char * ssid, const char * password, const char * ota_password)
{
    snprintf(s_ota_password, sizeof(s_ota_password), "%s", ota_password ? ota_password : "");

    setenv("TZ", TZ_AMSTERDAM, 1);
    tzset();
    sntp_set_time_sync_notification_cb(on_time_sync);
    configTzTime(TZ_AMSTERDAM, "pool.ntp.org", "time.google.com");

    WiFi.onEvent(on_wifi_event);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(EMS_HOSTNAME);
    // Geen modem-sleep: UDP-antwoorden van de Marstek (en HTTP-requests) mogen
    // niet vertraagd/gemist worden door een slapende radio.
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    // Standaard verbindt de ESP32 met het EERSTE access point met deze SSID dat
    // hij tegenkomt. Met meerdere access points (mesh/extender) bleef hij zo
    // aan een ver exemplaar hangen (-80 dBm, terwijl een ander op ~-55 stond).
    // Alle kanalen scannen en het sterkste kiezen.
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    WiFi.begin(ssid, password);
    EMS_LOGI(TAG, "verbinden met WiFi '%s'...", ssid);
}

bool net_connected(void)
{
    return WiFi.status() == WL_CONNECTED;
}

bool net_time_valid(void)
{
    return s_time_valid;
}

bool net_wait_for_time(unsigned long timeout_ms)
{
    unsigned long start = millis();
    while(!s_time_valid && millis() - start < timeout_ms) {
        delay(100);
    }
    return s_time_valid;
}
