#include "solar_forecast_client.h"

#include <cJSON.h>
#include <stdio.h>
#include <string.h>

#include "../net/http_client.h"

static const char * API_BASE_URL = "https://api.forecast.solar/estimate";

bool solar_fetch_forecast(double lat, double lon, double declination, double azimuth, double kwp, solar_row_t * rows,
                          int max_rows, int * n_rows, char * reason, int reason_size)
{
    *n_rows = 0;
    // %.10g geeft dezelfde getallen als Python's str(float) voor deze waarden
    // (bv. 51.96655765, 15, 3.6); de API accepteert "15" en "15.0" gelijk.
    char url[192];
    snprintf(url, sizeof(url), "%s/%.10g/%.10g/%.10g/%.10g/%.10g", API_BASE_URL, lat, lon, declination, azimuth, kwp);

    http_response_t r = http_get(url, SOLAR_REQUEST_TIMEOUT_MS, nullptr, "application/json");
    if(r.status < 0) {
        snprintf(reason, reason_size, "could not reach Forecast.Solar API");
        return false;
    }
    if(r.status != 200) {
        snprintf(reason, reason_size, "Forecast.Solar API returned HTTP %d", r.status);
        http_response_free(&r);
        return false;
    }
    cJSON * data = cJSON_Parse(r.body);
    http_response_free(&r);
    if(!data) {
        snprintf(reason, reason_size, "unexpected Forecast.Solar response: not JSON");
        return false;
    }

    const cJSON * result = cJSON_GetObjectItemCaseSensitive(data, "result");
    const cJSON * watts = result ? cJSON_GetObjectItemCaseSensitive(result, "watts") : nullptr;
    if(!cJSON_IsObject(watts)) {
        snprintf(reason, reason_size, "unexpected Forecast.Solar response shape");
        cJSON_Delete(data);
        return false;
    }

    // Zonsopkomst/-ondergang liggen niet op hele uren, dus meerdere tijdstempels
    // kunnen in hetzelfde uur vallen - laatst geziene waarde per (datum, uur)
    // wint, net als de dict-comprehension in de Python-versie (die behoudt de
    // positie van de EERSTE sleutel, maar de waarde van de LAATSTE).
    const cJSON * item;
    cJSON_ArrayForEach(item, watts)
    {
        int y, mo, d, h, mi, s;
        if(!cJSON_IsNumber(item) || sscanf(item->string, "%4d-%2d-%2d %2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6) {
            continue;
        }
        char date[11];
        snprintf(date, sizeof(date), "%04d-%02d-%02d", y, mo, d);
        int idx = -1;
        for(int i = 0; i < *n_rows; i++) {
            if(rows[i].hour == h && strcmp(rows[i].date, date) == 0) {
                idx = i;
                break;
            }
        }
        if(idx < 0) {
            if(*n_rows == max_rows) {
                continue;
            }
            idx = (*n_rows)++;
            memcpy(rows[idx].date, date, sizeof(date));
            rows[idx].hour = h;
        }
        rows[idx].watts = item->valuedouble;
    }
    cJSON_Delete(data);
    return true;
}
