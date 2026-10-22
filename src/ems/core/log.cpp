#include "log.h"

#include <stdarg.h>
#include <string.h>
#include <time.h>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t s_log_mutex = xSemaphoreCreateMutex();

// Laatste ~16 KB log in een ringbuffer (PSRAM), op te vragen via /api/log -
// voor als er geen seriele kabel aan het bord hangt.
static constexpr size_t RING_SIZE = 16 * 1024;
static char * s_ring = nullptr;
static size_t s_ring_head = 0; // volgende schrijfpositie
static bool s_ring_full = false;

static void ring_append(const char * text)
{
    if(!s_ring) {
        s_ring = (char *) heap_caps_malloc(RING_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if(!s_ring) return;
    }
    for(const char * p = text; *p; p++) {
        s_ring[s_ring_head++] = *p;
        if(s_ring_head == RING_SIZE) {
            s_ring_head = 0;
            s_ring_full = true;
        }
    }
}

void ems_log_copy(void (*write)(const char * data, size_t len, void * ctx), void * ctx)
{
    xSemaphoreTake(s_log_mutex, portMAX_DELAY);
    if(s_ring) {
        if(s_ring_full) write(s_ring + s_ring_head, RING_SIZE - s_ring_head, ctx);
        write(s_ring, s_ring_head, ctx);
    }
    xSemaphoreGive(s_log_mutex);
}

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

    char line[320];
    snprintf(line, sizeof(line), "%s %s %s %s\n", ts, name, level, msg);
    xSemaphoreTake(s_log_mutex, portMAX_DELAY);
    Serial.print(line);
    ring_append(line);
    xSemaphoreGive(s_log_mutex);
}
