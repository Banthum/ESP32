// WiFi (station), NTP-tijd in Europe/Amsterdam, mDNS en draadloos flashen.
#pragma once

#include <stdbool.h>

// Website en API op http://bmsmachine.local/. Twee apparaten met dezelfde
// mDNS-naam maken elkaar onvindbaar - een tweede kastje in hetzelfde netwerk
// heeft dus een eigen achtervoegsel nodig.
#define BMS_HOSTNAME "bmsmachine"

// Start WiFi met de gegeven gegevens (blokkeert niet), plus NTP/mDNS/OTA zodra
// er verbinding is. Herverbindt zelf na uitval.
void net_start(const char * ssid, const char * password, const char * ota_password);

bool net_connected(void);
// true zodra NTP de klok minstens één keer gezet heeft - tot dan zijn
// lokale datum/uur onbetrouwbaar (alles wat per dag/uur werkt moet wachten).
bool net_time_valid(void);
// Blokkeert tot net_time_valid() of timeout_ms verstreken; geeft de status terug.
bool net_wait_for_time(unsigned long timeout_ms);
