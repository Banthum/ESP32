// Port van dashboard/price_client.py: EnergyZero (uurprijzen elektra/gas,
// incl. BTW) en dynamisch-tarief.nl (echte 15-min EPEX, excl. BTW * 1.21).
#pragma once

#include <stdbool.h>

#include "../core/timeutil.h"

#define PRICE_REQUEST_TIMEOUT_MS 5000
#define PRICE_MAX_ATTEMPTS 3
#define DYNAMISCH_TARIEF_BTW_FACTOR 1.21
#define DYNAMISCH_TARIEF_DEFAULT_HOURS_AHEAD 36

// Ruim genoeg voor "vanaf nu + 36 u" (max ~240 kwartieren na lokale middernacht).
#define QUARTER_PRICE_MAX 320

// 24 EUR/kWh-waarden per lokaal uur van `local_date`; false als de API faalde
// of niet alle 24 lokale uren dekte ("geen data is beter dan nepdata").
bool price_fetch_price_curve(ems_date_t local_date, double out[24]);
// Gas in EUR/m3 (EnergyZero's eigen eenheid), zelfde contract.
bool price_fetch_gas_price_curve(ems_date_t local_date, double out[24]);

// {kwartier-sinds-lokale-middernacht-vandaag: EUR/kWh incl. BTW}, als
// valid[]/value[]-paar van QUARTER_PRICE_MAX lang. false als de request faalde
// of niets bruikbaars opleverde; een gedeeltelijke dekking is GEEN fout.
typedef struct {
    bool valid[QUARTER_PRICE_MAX];
    double value[QUARTER_PRICE_MAX];
} quarter_price_curve_t;

bool price_fetch_quarter_price_curve(int hours_ahead, quarter_price_curve_t * out);
