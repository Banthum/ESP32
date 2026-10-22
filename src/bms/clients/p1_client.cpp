#include "p1_client.h"

#include <ESPmDNS.h>
#include <cJSON.h>
#include <stdio.h>
#include <string.h>

#include "../config/config.h"
#include "../core/log.h"
#include "../core/paths.h"
#include "../net/http_client.h"

static const char * TAG = "dashboard.p1_client";

// (DISCOVER_TIMEOUT_S=6 op de Pi; ESPmDNS.queryService() gebruikt een vaste
// interne querytijd van ~3 s - geen parameter in deze core.)
static const int REQUEST_TIMEOUT_MS = 3000;

typedef struct {
    bool known;
    char ip[40];
    int port;
    char path[32];
} p1_device_t;

static p1_device_t s_cached = {};

// Zaait het adres uit de door de onboarding opgeslagen keuze, als die een
// WiFi-P1-meter vastlegde - zie _device_from_p1_source_config().
static bool device_from_p1_source_config(p1_device_t * dev)
{
    cJSON * config = config_load_json(BMS_P1_SOURCE_CONFIG_PATH);
    if(!config) {
        return false;
    }
    const cJSON * source = cJSON_GetObjectItemCaseSensitive(config, "source");
    const cJSON * ip = cJSON_GetObjectItemCaseSensitive(config, "ip");
    const cJSON * port = cJSON_GetObjectItemCaseSensitive(config, "port");
    const cJSON * path = cJSON_GetObjectItemCaseSensitive(config, "path");
    bool ok = cJSON_IsString(source) && strcmp(source->valuestring, "wifi_p1") == 0 && cJSON_IsString(ip) &&
              cJSON_IsNumber(port) && cJSON_IsString(path);
    if(ok) {
        dev->known = true;
        snprintf(dev->ip, sizeof(dev->ip), "%s", ip->valuestring);
        dev->port = port->valueint;
        snprintf(dev->path, sizeof(dev->path), "%s", path->valuestring);
    }
    cJSON_Delete(config);
    return ok;
}

bool p1_discover(char * ip, int ip_size, int * port, char * path, int path_size)
{
    // _hwenergy._tcp - zelfde servicetype als p1_client.SERVICE_TYPE.
    int n = MDNS.queryService("hwenergy", "tcp");
    for(int i = 0; i < n; i++) {
        if(MDNS.txt(i, "api_enabled") != "1") {
            continue;
        }
        String txt_path = MDNS.txt(i, "path");
        snprintf(ip, ip_size, "%s", MDNS.address(i).toString().c_str());
        *port = MDNS.port(i);
        snprintf(path, path_size, "%s", txt_path.length() ? txt_path.c_str() : "/api/v1");
        return true;
    }
    return false;
}

static bool discover(p1_device_t * dev)
{
    dev->known = p1_discover(dev->ip, sizeof(dev->ip), &dev->port, dev->path, sizeof(dev->path));
    return dev->known;
}

static bool fetch_power_w(const p1_device_t * dev, double * out)
{
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d%s/data", dev->ip, dev->port, dev->path);
    http_response_t r = http_get(url, REQUEST_TIMEOUT_MS, nullptr, "application/json");
    if(r.status != 200) {
        BMS_LOGW(TAG, "could not reach HomeWizard P1 meter at %s: HTTP %d", dev->ip, r.status);
        http_response_free(&r);
        return false;
    }
    cJSON * data = cJSON_Parse(r.body);
    http_response_free(&r);
    if(!data) {
        BMS_LOGW(TAG, "unexpected HomeWizard P1 response: not JSON");
        return false;
    }
    const cJSON * power = cJSON_GetObjectItemCaseSensitive(data, "active_power_w");
    bool ok = cJSON_IsNumber(power);
    if(ok) {
        *out = power->valuedouble;
    }
    cJSON_Delete(data);
    return ok;
}

bool p1_read_grid_power_w(double * out_w)
{
    if(!s_cached.known) {
        if(!device_from_p1_source_config(&s_cached) && !discover(&s_cached)) {
            BMS_LOGW(TAG, "no HomeWizard P1 meter known or found on the network");
            return false;
        }
    }
    if(fetch_power_w(&s_cached, out_w)) {
        return true;
    }
    BMS_LOGI(TAG, "cached HomeWizard P1 meter unreachable, rediscovering");
    if(!discover(&s_cached)) {
        return false;
    }
    return fetch_power_w(&s_cached, out_w);
}

const char * p1_current_ip(void)
{
    return s_cached.known ? s_cached.ip : "";
}
