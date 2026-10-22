#include "price_client.h"

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/log.h"
#include "../net/http_client.h"

static const char * TAG = "dashboard.price_client";

static const char * API_URL = "https://api.energyzero.nl/v1/energyprices";
static const char * DYNAMISCH_TARIEF_URL = "https://www.dynamisch-tarief.nl/api/stroom/kwartierprijzen";
// Zonder eigen User-Agent serveert dynamisch-tarief.nl de HTML-landingspagina
// i.p.v. JSON (live gevonden op de Pi, 2026-09-30) - eerlijk identificeren.
static const char * USER_AGENT = "BMSmachine/1.0 (+https://github.com/Banthum/EMS)";

static const int USAGE_TYPE_ELECTRICITY = 1;
static const int USAGE_TYPE_GAS = 2;

// "+" en ":" in de ISO-tijdstempels moeten URL-gecodeerd (zoals urlencode()).
static void url_encode_iso(const char * in, char * out, size_t out_size)
{
    size_t o = 0;
    for(const char * p = in; *p && o + 4 < out_size; p++) {
        if(*p == '+' || *p == ':') {
            o += snprintf(out + o, out_size - o, "%%%02X", (unsigned char) *p);
        } else {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
}

static bool fetch_energyzero_curve(int usage_type, ems_date_t local_date, double out[24])
{
    time_t local_start = local_midnight(local_date);
    time_t local_end = local_midnight(date_add_days(local_date, 1));

    char from_iso[32], till_iso[32], from_enc[48], till_enc[48];
    iso_utc(local_start, from_iso, sizeof(from_iso));
    iso_utc(local_end, till_iso, sizeof(till_iso));
    url_encode_iso(from_iso, from_enc, sizeof(from_enc));
    url_encode_iso(till_iso, till_enc, sizeof(till_enc));

    char url[256];
    snprintf(url, sizeof(url), "%s?fromDate=%s&tillDate=%s&interval=4&usageType=%d&inclBtw=true", API_URL, from_enc,
             till_enc, usage_type);

    cJSON * data = http_get_json(url, PRICE_REQUEST_TIMEOUT_MS, PRICE_MAX_ATTEMPTS, nullptr, "EnergyZero API");
    if(!data) {
        return false;
    }
    const cJSON * entries = cJSON_GetObjectItemCaseSensitive(data, "Prices");
    if(!cJSON_IsArray(entries) || cJSON_GetArraySize(entries) == 0) {
        EMS_LOGW(TAG, "EnergyZero response had no Prices entries");
        cJSON_Delete(data);
        return false;
    }

    // EnergyZero is eind-inclusief (25 entries voor 24 uur): de 25e is morgen
    // uur 0. Filteren op lokale DATUM, niet alleen uur - zie price_client.py.
    bool have[24] = {};
    int count = 0;
    const cJSON * entry;
    cJSON_ArrayForEach(entry, entries)
    {
        const cJSON * reading = cJSON_GetObjectItemCaseSensitive(entry, "readingDate");
        const cJSON * price = cJSON_GetObjectItemCaseSensitive(entry, "price");
        time_t t;
        if(!cJSON_IsString(reading) || !cJSON_IsNumber(price) || !parse_iso8601(reading->valuestring, &t)) {
            continue;
        }
        struct tm local;
        localtime_r(&t, &local);
        ems_date_t d = {local.tm_year + 1900, local.tm_mon + 1, local.tm_mday};
        if(!date_equal(d, local_date)) {
            continue;
        }
        if(!have[local.tm_hour]) {
            count++;
        }
        have[local.tm_hour] = true;
        out[local.tm_hour] = price->valuedouble;
    }
    cJSON_Delete(data);

    if(count < 24) {
        EMS_LOGW(TAG, "EnergyZero response only covered %d/24 local hours", count);
        return false;
    }
    return true;
}

bool price_fetch_price_curve(ems_date_t local_date, double out[24])
{
    return fetch_energyzero_curve(USAGE_TYPE_ELECTRICITY, local_date, out);
}

bool price_fetch_gas_price_curve(ems_date_t local_date, double out[24])
{
    return fetch_energyzero_curve(USAGE_TYPE_GAS, local_date, out);
}

bool price_fetch_quarter_price_curve(int hours_ahead, quarter_price_curve_t * out)
{
    memset(out, 0, sizeof(*out));
    time_t today_midnight = local_midnight(date_today());

    char url[160];
    snprintf(url, sizeof(url), "%s?uren=%d", DYNAMISCH_TARIEF_URL, hours_ahead);
    cJSON * data = http_get_json(url, PRICE_REQUEST_TIMEOUT_MS, PRICE_MAX_ATTEMPTS, USER_AGENT, "dynamisch-tarief.nl API");
    if(!data) {
        return false;
    }
    const cJSON * entries = cJSON_GetObjectItemCaseSensitive(data, "prices");
    if(!cJSON_IsArray(entries) || cJSON_GetArraySize(entries) == 0) {
        EMS_LOGW(TAG, "dynamisch-tarief.nl response had no prices entries");
        cJSON_Delete(data);
        return false;
    }

    int found = 0;
    const cJSON * entry;
    cJSON_ArrayForEach(entry, entries)
    {
        const cJSON * from = cJSON_GetObjectItemCaseSensitive(entry, "from");
        const cJSON * market = cJSON_GetObjectItemCaseSensitive(entry, "market_price");
        time_t start;
        if(!cJSON_IsString(from) || !parse_iso8601(from->valuestring, &start)) {
            continue;
        }
        double market_price;
        if(cJSON_IsNumber(market)) {
            market_price = market->valuedouble;
        } else if(cJSON_IsString(market)) {
            // float("0.123") in Python accepteert ook strings
            char * end = nullptr;
            market_price = strtod(market->valuestring, &end);
            if(end == market->valuestring) {
                continue;
            }
        } else {
            continue;
        }
        long diff = (long) (start - today_midnight);
        // Python: int(total_seconds // 900) - floor-deling
        long q = diff >= 0 ? diff / 900 : -((-diff + 899) / 900);
        if(q < 0 || q >= QUARTER_PRICE_MAX) {
            continue;
        }
        out->valid[q] = true;
        out->value[q] = market_price * DYNAMISCH_TARIEF_BTW_FACTOR;
        found++;
    }
    cJSON_Delete(data);

    if(found == 0) {
        EMS_LOGW(TAG, "dynamisch-tarief.nl response had no usable quarters after parsing");
        return false;
    }
    return true;
}
