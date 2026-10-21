#include <Arduino.h>

#include "solar_settings.h"

#include <cJSON.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/paths.h"
#include "../net/http_client.h"
#include "config.h"

static const char * PDOK_URL = "https://api.pdok.nl/bzk/locatieserver/search/v3_1/free";
static const int REQUEST_TIMEOUT_MS = 10000;

// Handmatig in sync met display/solar_forecast.py's RICHTING_MAP (alleen de
// sleutels zijn hier nodig voor de validatie - dec/az komen uit de UI).
static const char * const RICHTING_KEYS[] = {"zuid", "zuidoost", "zuidwest", "oost", "west", "plat"};

// urllib.parse.urlencode-equivalent voor één waarde (spatie -> '+').
static void url_encode(const char * in, char * out, size_t out_size)
{
    size_t o = 0;
    for(const unsigned char * p = (const unsigned char *) in; *p && o + 4 < out_size; p++) {
        if(isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[o++] = (char) *p;
        } else if(*p == ' ') {
            out[o++] = '+';
        } else {
            o += snprintf(out + o, out_size - o, "%%%02X", *p);
        }
    }
    out[o] = '\0';
}

bool solar_geocode(const char * postcode, const char * huisnummer, double * lat, double * lon, char * error_out,
                   size_t error_out_size)
{
    char query[64], encoded[160], url[256];
    snprintf(query, sizeof(query), "%s %s", postcode, huisnummer);
    url_encode(query, encoded, sizeof(encoded));
    snprintf(url, sizeof(url), "%s?q=%s&rows=1", PDOK_URL, encoded);

    http_response_t r = http_get(url, REQUEST_TIMEOUT_MS, nullptr, "application/json");
    if(r.status != 200) {
        snprintf(error_out, error_out_size, "kon PDOK niet bereiken: HTTP %d", r.status);
        http_response_free(&r);
        return false;
    }
    cJSON * data = cJSON_Parse(r.body);
    http_response_free(&r);
    if(!data) {
        snprintf(error_out, error_out_size, "onverwacht antwoord van PDOK: geen JSON");
        return false;
    }
    const cJSON * response = cJSON_GetObjectItem(data, "response");
    const cJSON * docs = response ? cJSON_GetObjectItem(response, "docs") : nullptr;
    if(!cJSON_IsArray(docs) || cJSON_GetArraySize(docs) == 0) {
        snprintf(error_out, error_out_size, "geen adres gevonden voor %s", query);
        cJSON_Delete(data);
        return false;
    }
    const cJSON * centroide = cJSON_GetObjectItem(cJSON_GetArrayItem(docs, 0), "centroide_ll");
    // "POINT(<lon> <lat>)"
    if(!cJSON_IsString(centroide) || sscanf(centroide->valuestring, "POINT(%lf %lf)", lon, lat) != 2) {
        snprintf(error_out, error_out_size, "kon PDOK-coordinaat niet parsen: %s",
                 cJSON_IsString(centroide) ? centroide->valuestring : "");
        cJSON_Delete(data);
        return false;
    }
    cJSON_Delete(data);
    return true;
}

// int(s) / float(s) van Python: hele string moet een getal zijn (witruimte mag).
static bool parse_int(const char * s, long * out)
{
    char * end;
    long v = strtol(s, &end, 10);
    while(isspace((unsigned char) *end)) end++;
    if(end == s || *end != '\0') return false;
    *out = v;
    return true;
}

static bool parse_float(const char * s, double * out)
{
    char * end;
    double v = strtod(s, &end);
    while(isspace((unsigned char) *end)) end++;
    if(end == s || *end != '\0') return false;
    *out = v;
    return true;
}

bool solar_settings_write(const char * postcode, const char * huisnummer, const char * panel_count_str,
                          const char * panel_wp_str, const char * richting, const char * forecast_factor_str,
                          const char * declination_str, const char * azimuth_str, char * error_out,
                          size_t error_out_size)
{
    bool known = false;
    for(const char * k : RICHTING_KEYS) known = known || strcmp(k, richting) == 0;
    if(!known) {
        snprintf(error_out, error_out_size, "onbekende dakorientatie: %s", richting);
        return false;
    }
    long panel_count;
    if(!parse_int(panel_count_str, &panel_count) || panel_count <= 0) {
        snprintf(error_out, error_out_size, "ongeldig aantal panelen: %s", panel_count_str);
        return false;
    }
    double panel_wp;
    if(!parse_float(panel_wp_str, &panel_wp) || panel_wp <= 0) {
        snprintf(error_out, error_out_size, "ongeldig Wp-per-paneel-getal: %s", panel_wp_str);
        return false;
    }
    double kwp = panel_count * panel_wp / 1000.0;
    double factor;
    if(!parse_float(forecast_factor_str, &factor) || factor <= 0) {
        snprintf(error_out, error_out_size, "ongeldige kalibratiefactor: %s", forecast_factor_str);
        return false;
    }
    double declination;
    if(!parse_float(declination_str, &declination) || declination < 0 || declination > 90) {
        snprintf(error_out, error_out_size, "ongeldige kantelhoek (0-90): %s", declination_str);
        return false;
    }
    double azimuth;
    if(!parse_float(azimuth_str, &azimuth) || azimuth < -180 || azimuth > 180) {
        snprintf(error_out, error_out_size, "ongeldige azimuth (-180 tot 180): %s", azimuth_str);
        return false;
    }

    double lat, lon;
    if(!solar_geocode(postcode, huisnummer, &lat, &lon, error_out, error_out_size)) {
        return false;
    }

    cJSON * config = cJSON_CreateObject();
    cJSON_AddStringToObject(config, "postcode", postcode);
    cJSON_AddStringToObject(config, "huisnummer", huisnummer);
    cJSON_AddNumberToObject(config, "panel_count", panel_count);
    cJSON_AddNumberToObject(config, "panel_wp", panel_wp);
    cJSON_AddNumberToObject(config, "kwp", kwp);
    cJSON_AddStringToObject(config, "richting", richting);
    cJSON_AddNumberToObject(config, "declination", declination);
    cJSON_AddNumberToObject(config, "azimuth", azimuth);
    cJSON_AddNumberToObject(config, "lat", lat);
    cJSON_AddNumberToObject(config, "lon", lon);
    cJSON_AddNumberToObject(config, "forecast_factor", factor);
    bool ok = config_write_json(EMS_SOLAR_CONFIG_PATH, config);
    cJSON_Delete(config);
    if(!ok) {
        snprintf(error_out, error_out_size, "kon %s niet wegschrijven", EMS_SOLAR_CONFIG_PATH);
    }
    return ok;
}

bool solar_settings_write_no_panels(char * error_out, size_t error_out_size)
{
    cJSON * config = cJSON_CreateObject();
    cJSON_AddBoolToObject(config, "no_solar_panels", true);
    bool ok = config_write_json(EMS_SOLAR_CONFIG_PATH, config);
    cJSON_Delete(config);
    if(!ok) {
        snprintf(error_out, error_out_size, "kon %s niet wegschrijven", EMS_SOLAR_CONFIG_PATH);
    }
    return ok;
}
