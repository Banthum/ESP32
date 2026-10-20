// Minimale logging naar Serial, zelfde rol als Python's logging-module in
// dashboard/*.py: "<tijd> <naam> <niveau> <bericht>".
#pragma once

#include <Arduino.h>

void ems_log(const char * level, const char * name, const char * fmt, ...) __attribute__((format(printf, 3, 4)));

#define EMS_LOGI(name, ...) ems_log("INFO", name, __VA_ARGS__)
#define EMS_LOGW(name, ...) ems_log("WARNING", name, __VA_ARGS__)
#define EMS_LOGE(name, ...) ems_log("ERROR", name, __VA_ARGS__)
