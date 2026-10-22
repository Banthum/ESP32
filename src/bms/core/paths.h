// Bestandslocaties op de ESP32 - vervangen /etc/emsmachine/ (config, DB) en
// /home/pi/ (CSV-exports, default-profiel) van de Pi. Alles op LittleFS,
// gemount op /littlefs zodat gewone fopen()/cJSON-code zoals in
// dashboard-ui/src/screen_*.c ongewijzigd blijft werken.
#pragma once

#define BMS_FS_MOUNT "/littlefs"
#define BMS_DIR BMS_FS_MOUNT "/ems"

#define BMS_BATTERY_CONFIG_PATH BMS_DIR "/battery_config.json"
#define BMS_GAS_CONFIG_PATH BMS_DIR "/gas_config.json"
#define BMS_SOLAR_CONFIG_PATH BMS_DIR "/solar_config.json"
#define BMS_P1_SOURCE_CONFIG_PATH BMS_DIR "/p1_source.json"
#define BMS_MARSTEK_CONFIG_PATH BMS_DIR "/marstek.json"
#define BMS_WIFI_CONFIG_PATH BMS_DIR "/wifi.json"
#define BMS_DB_PATH BMS_DIR "/bmsmachine.db"
#define BMS_DB_LEGACY_PATH BMS_DIR "/emsmachine.db" // naam vóór BMSmachine; main.cpp hernoemt hem
#define BMS_DEFAULT_CONSUMPTION_PROFILE_PATH BMS_DIR "/default_consumption_profile.csv"
