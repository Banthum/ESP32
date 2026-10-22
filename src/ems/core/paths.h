// Bestandslocaties op de ESP32 - vervangen /etc/emsmachine/ (config, DB) en
// /home/pi/ (CSV-exports, default-profiel) van de Pi. Alles op LittleFS,
// gemount op /littlefs zodat gewone fopen()/cJSON-code zoals in
// dashboard-ui/src/screen_*.c ongewijzigd blijft werken.
#pragma once

#define EMS_FS_MOUNT "/littlefs"
#define EMS_DIR EMS_FS_MOUNT "/ems"

#define EMS_BATTERY_CONFIG_PATH EMS_DIR "/battery_config.json"
#define EMS_GAS_CONFIG_PATH EMS_DIR "/gas_config.json"
#define EMS_SOLAR_CONFIG_PATH EMS_DIR "/solar_config.json"
#define EMS_P1_SOURCE_CONFIG_PATH EMS_DIR "/p1_source.json"
#define EMS_MARSTEK_CONFIG_PATH EMS_DIR "/marstek.json"
#define EMS_WIFI_CONFIG_PATH EMS_DIR "/wifi.json"
#define EMS_DB_PATH EMS_DIR "/bmsmachine.db"
#define EMS_DB_LEGACY_PATH EMS_DIR "/emsmachine.db" // naam vóór BMSmachine; main.cpp hernoemt hem
#define EMS_DEFAULT_CONSUMPTION_PROFILE_PATH EMS_DIR "/default_consumption_profile.csv"
