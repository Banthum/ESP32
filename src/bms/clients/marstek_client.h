// Port van dashboard/marstek_client.py.
//
// De schrijfkant (Passive/Auto zetten) bestaat alleen in een build met
// BMS_BATTERY_CONTROL_LIVE=1. Zonder die vlag - de standaard, zolang de Pi de
// batterij nog aanstuurt - wordt hij niet eens meegecompileerd, en weigert
// tools/check_readonly.py de build als de schrijfkant ergens anders opduikt.
#pragma once

#include <cJSON.h>
#include <stdbool.h>

#ifndef BMS_BATTERY_CONTROL_LIVE
#define BMS_BATTERY_CONTROL_LIVE 0
#endif

#define MARSTEK_PORT 30000
#define MARSTEK_TIMEOUT_MS 2000
#define MARSTEK_MAX_ATTEMPTS 3
// Deadband rond 0 W voor "idle" - zelfde waarde als marstek_client.IDLE_DEADBAND_W.
#define MARSTEK_IDLE_DEADBAND_W 20

typedef struct {
    bool battery_power_w_valid;
    double battery_power_w; // + = ontladen, - = laden (ongrid_power, zie marstek_client.py)
    const char * battery_state; // "charging"/"discharging"/"idle", of NULL
    bool battery_soc_pct_valid;
    double battery_soc_pct;
    bool solar_power_w_valid;
    double solar_power_w;
    bool grid_power_w_valid; // zelfde ongrid_power, als terugval voor P1
    double grid_power_w;
} marstek_telemetry_t;

typedef struct {
    bool valid;
    char mode[16]; // "Auto"/"Passive"/...
    bool ongrid_power_valid;
    double ongrid_power;
} marstek_mode_t;

// Eén Bat.GetStatus + één ES.GetStatus (zie read_telemetry()).
marstek_telemetry_t marstek_read_telemetry(const char * host);

// ES.GetMode: de modus die de batterij zelf meldt, om een commando te
// verifiëren (zie get_mode() in marstek_client.py).
marstek_mode_t marstek_get_mode(const char * host);

#if BMS_BATTERY_CONTROL_LIVE
// Passive op een vast vermogen (- = laden) voor duration_s seconden, of terug
// naar Auto. true = de batterij bevestigde het commando. Nooit ontdubbeld:
// de aanroeper (battery_control) beslist wanneer opnieuw sturen nodig is.
bool marstek_write_passive_mode(const char * host, double power_w, double duration_s);
bool marstek_write_auto_mode(const char * host);
#endif

// Snapshot van de per-methode tellers {"Bat.GetStatus": {"success": n, ...}}
// (aanroeper doet cJSON_Delete), of NULL als er niets geteld is.
cJSON * marstek_get_call_stats(void);
void marstek_reset_call_stats(void);
