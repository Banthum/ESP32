// Port van dashboard/battery_config.py, dashboard/gas_config.py en
// solar_forecast_client.read_solar_config(): zelfde defaults, zelfde
// "nooit crashen, per-veld terugvallen op de default"-validatie.
#pragma once

#include <cJSON.h>
#include <stdbool.h>

// --- battery_config.py ---
#define BATTERY_DEFAULT_CAPACITY_KWH 5.12
#define BATTERY_DEFAULT_MAX_POWER_W 2500.0
// Op de Pi standaard aan; op de ESP32 bestaat aansturing niet - de waarde
// wordt wel gelezen/bewaard zodat een gekopieerde config niet verloren gaat,
// maar daemon.cpp stuurt altijd false mee (zie batterij-alleen-lezen-principe).
#define BATTERY_DEFAULT_ACTIVE_CONTROL_ENABLED true
#define BATTERY_DEFAULT_MIN_SOC_PCT 12.0
#define BATTERY_DEFAULT_MAX_SOC_PCT 100.0
#define BATTERY_DEFAULT_ENERGY_LOSS_PCT 5.0

typedef struct {
    double capacity_kwh;
    double max_power_w;
    bool active_control_enabled;
    double min_soc_pct;
    double max_soc_pct;
    double energy_loss_pct;
} battery_config_t;

battery_config_t config_read_battery(const char * path);
double config_read_battery_capacity_kwh(const char * path);

// --- gas_config.py ---
#define GAS_KWH_PER_M3 9.769
#define GAS_DEFAULT_ENABLED true

typedef struct {
    bool enabled;
    bool unit_is_m3; // false = "kwh" (default), true = "m3"
} gas_config_t;

gas_config_t config_read_gas(const char * path);

// --- solar_forecast_client.read_solar_config() ---
// Geeft de geparste JSON terug (aanroeper doet cJSON_Delete), of NULL als het
// bestand niet bestaat/onleesbaar is ("onboarding niet gedaan").
cJSON * config_read_solar(const char * path);

// --- Marstek-batterij-IP ---
// Op de Pi hardcoded (marstek_client.DEFAULT_HOST); hier door de onboarding
// weggeschreven, met dezelfde dev-default als terugval.
#define MARSTEK_DEFAULT_HOST "192.168.2.144"
void config_read_marstek_host(const char * path, char * out, size_t out_size);

// Gedeelde helpers
char * config_read_file(const char * path);            // malloc'd, NULL bij fout
cJSON * config_load_json(const char * path);            // NULL bij ontbreken/fout
bool config_write_json(const char * path, const cJSON * json);
