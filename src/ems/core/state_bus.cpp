#include "state_bus.h"

#include <stdlib.h>
#include <string.h>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t s_mutex = xSemaphoreCreateMutex();
static char * s_json = nullptr;
static uint32_t s_version = 0;

void state_bus_publish(const char * json)
{
    size_t len = strlen(json);
    // Een volledig bericht is ~15-25 KB (vele 192-punts arrays) - PSRAM.
    char * copy = (char *) heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if(!copy) {
        return;
    }
    memcpy(copy, json, len + 1);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    char * old = s_json;
    s_json = copy;
    s_version++;
    xSemaphoreGive(s_mutex);
    free(old);
}

char * state_bus_copy(uint32_t * version_out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    char * copy = nullptr;
    if(s_json) {
        size_t len = strlen(s_json);
        copy = (char *) heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
        if(copy) {
            memcpy(copy, s_json, len + 1);
        }
    }
    if(version_out) {
        *version_out = s_version;
    }
    xSemaphoreGive(s_mutex);
    return copy;
}

uint32_t state_bus_version(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t v = s_version;
    xSemaphoreGive(s_mutex);
    return v;
}
