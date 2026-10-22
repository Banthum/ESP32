#include "net.h"

#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <string.h>
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
static char s_ssid[33];
static char s_password[65];

// Roaming. Bij het verbinden kiest de ESP32 het sterkste access point, maar
// daarna blijft hij hangen, ook als dat AP ver weg blijkt (live 2026-10-22: na
// een herstart -83 dBm op het verre AP terwijl het nabije op -56 stond - en
// dan mislukken UDP-commando's aan de batterij vaker). Bij zwak signaal
// scannen en overstappen naar een AP met dezelfde SSID dat duidelijk sterker is.
static const uint32_t ROAM_FIRST_CHECK_MS = 30 * 1000;
static const uint32_t ROAM_CHECK_INTERVAL_MS = 2 * 60 * 1000;
static const int ROAM_RSSI_THRESHOLD_DBM = -70;
static const int ROAM_MIN_GAIN_DB = 10;
// Na een overstap is het AP vastgezet (BSSID); valt de verbinding weg, dan
// zonder vast BSSID opnieuw verbinden, anders blijft hij een verdwenen AP zoeken.
// Een overstap verbreekt zelf eerst de verbinding: die melding negeren we
// zolang de overstap loopt (s_roam_started_ms != 0), maar lukt hij niet binnen
// ROAM_TIMEOUT_MS, dan alsnog vrij verbinden.
static const uint32_t ROAM_TIMEOUT_MS = 20 * 1000;
static volatile bool s_bssid_locked = false;
static volatile bool s_unlock_requested = false;
static volatile uint32_t s_roam_started_ms = 0;

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
            s_roam_started_ms = 0;
            start_services();
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            EMS_LOGW(TAG, "WiFi verbroken (reden %d)", (int) info.wifi_sta_disconnected.reason);
            if(s_bssid_locked && s_roam_started_ms == 0) s_unlock_requested = true;
            break;
        default:
            break;
    }
}

static void try_roam(void)
{
    int rssi = WiFi.RSSI();
    if(rssi >= ROAM_RSSI_THRESHOLD_DBM) {
        return;
    }
    uint8_t current[6];
    memcpy(current, WiFi.BSSID(), 6);
    int n = WiFi.scanNetworks(false, false, false, 120);
    int best = -1;
    int best_rssi = -1000;
    for(int i = 0; i < n; i++) {
        if(WiFi.SSID(i) == s_ssid && memcmp(WiFi.BSSID(i), current, 6) != 0 && WiFi.RSSI(i) > best_rssi) {
            best = i;
            best_rssi = WiFi.RSSI(i);
        }
    }
    if(best < 0 || best_rssi < rssi + ROAM_MIN_GAIN_DB) {
        WiFi.scanDelete();
        return;
    }
    uint8_t bssid[6];
    memcpy(bssid, WiFi.BSSID(best), 6);
    int32_t channel = WiFi.channel(best);
    String target = WiFi.BSSIDstr(best);
    WiFi.scanDelete();
    EMS_LOGI(TAG, "roaming: %d dBm op %s -> %d dBm op %s (kanaal %d)", rssi, WiFi.BSSIDstr().c_str(), best_rssi,
             target.c_str(), (int) channel);
    s_bssid_locked = true;
    s_roam_started_ms = millis() | 1;
    WiFi.begin(s_ssid, s_password, channel, bssid);
}

static void roam_task(void * arg)
{
    (void) arg;
    uint32_t next_check = millis() + ROAM_FIRST_CHECK_MS;
    for(;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if(s_roam_started_ms != 0 && millis() - s_roam_started_ms > ROAM_TIMEOUT_MS) {
            EMS_LOGW(TAG, "roaming: overstap mislukt");
            s_roam_started_ms = 0;
            s_unlock_requested = true;
        }
        if(s_unlock_requested) {
            s_unlock_requested = false;
            s_bssid_locked = false;
            EMS_LOGI(TAG, "roaming: opnieuw verbinden zonder vast access point");
            WiFi.begin(s_ssid, s_password);
            next_check = millis() + ROAM_FIRST_CHECK_MS;
            continue;
        }
        if((int32_t) (millis() - next_check) < 0 || WiFi.status() != WL_CONNECTED) {
            continue;
        }
        next_check = millis() + ROAM_CHECK_INTERVAL_MS;
        try_roam();
    }
}

void net_start(const char * ssid, const char * password, const char * ota_password)
{
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_password, sizeof(s_password), "%s", password);
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
    xTaskCreatePinnedToCore(roam_task, "roam", 4096, nullptr, 1, nullptr, 0);
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
