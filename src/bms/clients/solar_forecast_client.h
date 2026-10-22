// Port van dashboard/solar_forecast_client.py: Forecast.Solar publieke API
// (geen key, 12 requests/uur per IP - de Pi en de ESP32 delen hetzelfde
// publieke IP, dus beide pollen bewust maar eens per 3 uur).
#pragma once

#include <stdbool.h>

#define SOLAR_REQUEST_TIMEOUT_MS 10000
#define SOLAR_MAX_ROWS 64

typedef struct {
    char date[11]; // "YYYY-MM-DD"
    int hour;
    double watts;
} solar_row_t;

// Haalt de voorspelling op (vandaag én morgen in één call). Bij succes true +
// rows/n_rows (laatste waarde per (datum, uur) wint, zoals de dict-comprehension
// in fetch_forecast()); bij falen false + een reden in `reason`.
bool solar_fetch_forecast(double lat, double lon, double declination, double azimuth, double kwp, solar_row_t * rows,
                          int max_rows, int * n_rows, char * reason, int reason_size);
