#include "log.h"

#include <stdarg.h>
#include <time.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t s_log_mutex = xSemaphoreCreateMutex();

void ems_log(const char * level, const char * name, const char * fmt, ...)
{
    char msg[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    char ts[24];
    time_t now = time(nullptr);
    if(now > 1700000000) {
        struct tm local;
        localtime_r(&now, &local);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &local);
    } else {
        snprintf(ts, sizeof(ts), "+%lu.%03lus", millis() / 1000, millis() % 1000);
    }

    xSemaphoreTake(s_log_mutex, portMAX_DELAY);
    Serial.printf("%s %s %s %s\n", ts, name, level, msg);
    xSemaphoreGive(s_log_mutex);
}
