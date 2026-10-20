#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../core/log.h"

static const char * TAG = "dashboard.config";

char * config_read_file(const char * path)
{
    FILE * f = fopen(path, "rb");
    if(!f) {
        return nullptr;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if(size < 0) {
        fclose(f);
        return nullptr;
    }
    char * buf = (char *) malloc(size + 1);
    if(!buf) {
        fclose(f);
        return nullptr;
    }
    size_t n = fread(buf, 1, size, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

// FileNotFound -> stil NULL; elke andere fout -> warning + NULL. Zelfde
// contract als battery_config._load_config().
cJSON * config_load_json(const char * path)
{
    struct stat st;
    if(stat(path, &st) != 0) {
        return nullptr;
    }
    char * text = config_read_file(path);
    if(!text) {
        EMS_LOGW(TAG, "could not read %s", path);
        return nullptr;
    }
    cJSON * json = cJSON_Parse(text);
    free(text);
    if(!json) {
        EMS_LOGW(TAG, "could not parse %s", path);
    }
    return json;
}

bool config_write_json(const char * path, const cJSON * json)
{
    char * text = cJSON_Print(json);
    if(!text) {
        return false;
    }
    // Eerst naar een tijdelijk bestand, dan rename - een stroomonderbreking
    // halverwege laat zo nooit een half geschreven config achter.
    char tmp_path[128];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE * f = fopen(tmp_path, "w");
    if(!f) {
        free(text);
        EMS_LOGW(TAG, "could not write %s: %s", tmp_path, strerror(errno));
        return false;
    }
    size_t len = strlen(text);
    bool ok = fwrite(text, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    free(text);
    if(!ok) {
        remove(tmp_path);
        return false;
    }
    remove(path);
    if(rename(tmp_path, path) != 0) {
        EMS_LOGW(TAG, "could not rename %s: %s", tmp_path, strerror(errno));
        return false;
    }
    return true;
}

static bool get_number(const cJSON * obj, const char * key, double * out)
{
    const cJSON * item = cJSON_GetObjectItemCaseSensitive(obj, key);
    // Python's isinstance(x, (int, float)) sluit bool uit in de praktijk
    // niet, maar json.load levert True/False nooit als getal - cJSON evenmin.
    if(!cJSON_IsNumber(item)) {
        return false;
    }
    *out = item->valuedouble;
    return true;
}

double config_read_battery_capacity_kwh(const char * path)
{
    cJSON * config = config_load_json(path);
    double capacity;
    bool valid = config && get_number(config, "capacity_kwh", &capacity) && capacity > 0;
    if(!valid) {
        if(config && cJSON_GetArraySize(config) > 0) {
            EMS_LOGW(TAG, "%s has no valid capacity_kwh, using default", path);
        }
        capacity = BATTERY_DEFAULT_CAPACITY_KWH;
    }
    cJSON_Delete(config);
    return capacity;
}

battery_config_t config_read_battery(const char * path)
{
    cJSON * config = config_load_json(path);
    bool has_config = config && cJSON_GetArraySize(config) > 0;
    battery_config_t out;

    double max_power;
    if(!(config && get_number(config, "max_power_w", &max_power) && max_power > 0)) {
        if(has_config && cJSON_HasObjectItem(config, "max_power_w")) {
            EMS_LOGW(TAG, "%s has invalid max_power_w, using default", path);
        }
        max_power = BATTERY_DEFAULT_MAX_POWER_W;
    }

    const cJSON * active = config ? cJSON_GetObjectItemCaseSensitive(config, "active_control_enabled") : nullptr;
    bool active_control;
    if(cJSON_IsBool(active)) {
        active_control = cJSON_IsTrue(active);
    } else {
        if(has_config && active) {
            EMS_LOGW(TAG, "%s has invalid active_control_enabled, using default", path);
        }
        active_control = BATTERY_DEFAULT_ACTIVE_CONTROL_ENABLED;
    }

    // Gevalideerd als trio, niet per veld - zie battery_config.py.
    double min_soc, max_soc, loss;
    bool valid = config && get_number(config, "min_soc_pct", &min_soc) && get_number(config, "max_soc_pct", &max_soc) &&
                 get_number(config, "energy_loss_pct", &loss) && 0 <= min_soc && min_soc < max_soc &&
                 max_soc <= 100 && 0 <= loss && loss < 100;
    if(!valid) {
        if(has_config && (cJSON_HasObjectItem(config, "min_soc_pct") || cJSON_HasObjectItem(config, "max_soc_pct") ||
                          cJSON_HasObjectItem(config, "energy_loss_pct"))) {
            EMS_LOGW(TAG, "%s has invalid/inconsistent min_soc_pct/max_soc_pct/energy_loss_pct, using defaults", path);
        }
        min_soc = BATTERY_DEFAULT_MIN_SOC_PCT;
        max_soc = BATTERY_DEFAULT_MAX_SOC_PCT;
        loss = BATTERY_DEFAULT_ENERGY_LOSS_PCT;
    }
    cJSON_Delete(config);

    out.capacity_kwh = config_read_battery_capacity_kwh(path);
    out.max_power_w = max_power;
    out.active_control_enabled = active_control;
    out.min_soc_pct = min_soc;
    out.max_soc_pct = max_soc;
    out.energy_loss_pct = loss;
    return out;
}

gas_config_t config_read_gas(const char * path)
{
    gas_config_t out = {GAS_DEFAULT_ENABLED, false};
    cJSON * raw = config_load_json(path);
    if(!raw) {
        return out;
    }
    const cJSON * enabled = cJSON_GetObjectItemCaseSensitive(raw, "enabled");
    if(cJSON_IsBool(enabled)) {
        out.enabled = cJSON_IsTrue(enabled);
    }
    const cJSON * unit = cJSON_GetObjectItemCaseSensitive(raw, "unit");
    if(cJSON_IsString(unit)) {
        if(strcmp(unit->valuestring, "kwh") == 0) {
            out.unit_is_m3 = false;
        } else if(strcmp(unit->valuestring, "m3") == 0) {
            out.unit_is_m3 = true;
        }
    }
    cJSON_Delete(raw);
    return out;
}

cJSON * config_read_solar(const char * path)
{
    return config_load_json(path);
}

void config_read_marstek_host(const char * path, char * out, size_t out_size)
{
    snprintf(out, out_size, "%s", MARSTEK_DEFAULT_HOST);
    cJSON * raw = config_load_json(path);
    if(!raw) {
        return;
    }
    const cJSON * ip = cJSON_GetObjectItemCaseSensitive(raw, "ip");
    if(cJSON_IsString(ip) && ip->valuestring[0] != '\0') {
        snprintf(out, out_size, "%s", ip->valuestring);
    }
    cJSON_Delete(raw);
}
