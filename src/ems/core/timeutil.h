// Datum/tijd-hulpjes ter vervanging van Python's datetime/zoneinfo. De
// tijdzone (Europe/Amsterdam) staat via setenv("TZ") in net.cpp, dus
// localtime_r/mktime rekenen in lokale tijd, net als de Pi.
#pragma once

#include <stdbool.h>
#include <time.h>

typedef struct {
    int year, month, day; // month 1-12
} ems_date_t;

ems_date_t date_today(void);
ems_date_t date_add_days(ems_date_t d, int days);
ems_date_t date_from_epoch_local(time_t t);
bool date_equal(ems_date_t a, ems_date_t b);
// "YYYY-MM-DD" (out >= 11 bytes)
void date_iso(ems_date_t d, char * out);
bool date_parse_iso(const char * s, ems_date_t * out);
// weekday() zoals Python: maandag = 0 ... zondag = 6
int date_weekday(ems_date_t d);

// Epoch van lokale middernacht van `d` (DST-correct via mktime).
time_t local_midnight(ems_date_t d);
// Epoch van lokale datum + uur:minuut.
time_t local_time_of(ems_date_t d, int hour, int minute);

// Epoch -> "YYYY-MM-DDTHH:MM:SS+00:00" (zoals Python's isoformat() in UTC).
void iso_utc(time_t t, char * out, int out_size);
// Lokale tijd met offset, zoals datetime.now(timezone.utc).isoformat() maar dan
// UTC - gebruikt voor het "timestamp"-veld van het dashboardbericht.
void iso_utc_now(char * out, int out_size);

// Parst ISO-8601 met 'Z' of '+HH:MM'/'-HH:MM' (fractie optioneel) naar epoch.
bool parse_iso8601(const char * s, time_t * out);

// UTC-epoch uit kalendervelden (timegm-equivalent).
time_t utc_epoch(int year, int month, int day, int hour, int minute, int second);
