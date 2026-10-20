// Port van dashboard/marstek_client.py - ALLEEN de leeskant.
//
// De Pi stuurt de batterij aan; deze ESP32 mag dat nooit (twee sturende
// systemen zouden elkaar tegenwerken). Daarom bestaan de twee schrijfhelpers
// uit marstek_client.py hier bewust niet, en weigert de build
// (tools/check_readonly.py) elke bron in src/ems/ die de schrijfkant van de
// Marstek-API noemt.
#pragma once

#include <cJSON.h>
#include <stdbool.h>

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

// ES.GetMode - alleen lezen. Op de ESP32 gebruikt om te herkennen dat de Pi
// een laadvenster aanstuurt (Passive), voor de besparingsboekhouding.
marstek_mode_t marstek_get_mode(const char * host);

// Snapshot van de per-methode tellers {"Bat.GetStatus": {"success": n, ...}}
// (aanroeper doet cJSON_Delete), of NULL als er niets geteld is.
cJSON * marstek_get_call_stats(void);
void marstek_reset_call_stats(void);
