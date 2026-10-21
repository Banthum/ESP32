// Port van dashboard/storage.py: SQLite (zelfde schema en SQL) op LittleFS.
//
// Bewuste afwijkingen t.o.v. de Pi (beperkte flash):
// - ruwe `measurements` worden na MEASUREMENT_RETENTION_DAYS opgeschoond
//   (op de Pi blijven ze voor altijd); alles wat de daemon leest
//   (aggregatie van gisteren, werkelijk verbruik van vandaag) valt ruim binnen.
// - CSV-exports gaan via HTTP (storage_export_*_csv) i.p.v. naar /home/pi/.
#pragma once

#include <cJSON.h>
#include <stdbool.h>
#include <time.h>

#include "../core/optional.h"
#include "../core/timeutil.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QUARTERS_PER_DAY 96
#define MEASUREMENT_RETENTION_DAYS 3
#define BATTERY_LOG_RETENTION_DAYS 14
#define DEFAULT_BASELINE_LOAD_W 100.0
#define OVERNIGHT_BASELINE_START_QUARTER 4
#define OVERNIGHT_BASELINE_END_QUARTER 12 // exclusief

// De database draait in PSRAM; db_path is het bestand op LittleFS waaruit hij
// bij init geladen en waarnaar hij door storage_persist() weggeschreven wordt.
#define STORAGE_PERSIST_INTERVAL_MS (60UL * 60UL * 1000UL)

bool storage_init(const char * db_path);
void storage_close(void); // schrijft eerst weg
// Hele database atomair naar LittleFS (tmp-bestand + rename).
bool storage_persist(void);
// storage_persist() als het laatste wegschrijven STORAGE_PERSIST_INTERVAL_MS geleden is.
void storage_persist_if_due(void);

// --- MeasurementBuffer (in-memory, flush = één gemiddelde per bron) ---
void storage_measurement_record(const char * source, double value, const char * unit);
void storage_measurement_flush(void);

// --- SavingsBuffer ---
void storage_savings_record(double batterij_eur, double ems_eur);
void storage_savings_pending(double * batterij_eur, double * ems_eur);
void storage_savings_flush(void);

// --- battery_pool_state (shadow-SOC, zie savings_tracker) ---
double storage_load_pool_state(void); // shadow_soc_kwh, 0.0 als nooit opgeslagen
void storage_save_pool_state(double shadow_soc_kwh);

// --- savings_summary(): dag/week/maand/jaar/altijd ---
typedef struct {
    double batterij_eur;
    double ems_eur;
} savings_pair_t;

typedef struct {
    savings_pair_t dag, week, maand, jaar, altijd;
} savings_summary_t;

savings_summary_t storage_savings_summary(ems_date_t today);

// --- aggregatie / voorspelling ---
int storage_aggregate_quarterly(const char * date_str, const char * source);
void storage_actual_consumption_curve(const char * source, ems_date_t date, opt_d out[QUARTERS_PER_DAY]);
// false = geen data in het 01:00-03:00-venster (Python: None)
bool storage_overnight_baseline_load_w(const char * date_str, double * out_w);

// default_consumption_profile.csv -> {kwartier: W}; geen bestand -> alles ongeldig.
void storage_load_default_consumption_profile(const char * path, opt_d profile[QUARTERS_PER_DAY]);

// expected_consumption(): voorspeld vermogen (W) op `timestamp`. `profile` mag NULL.
opt_d storage_expected_consumption(time_t timestamp, int lookback_days, int min_samples,
                                   const opt_d * profile);
// expected_consumption_curve(): 96 kWh-waarden per kwartier van `date`.
// Laadt het default-profiel zelf opnieuw van flash (zoals de Pi bij elke aanroep).
void storage_expected_consumption_curve(ems_date_t date, opt_d out[QUARTERS_PER_DAY]);

// --- zonvoorspelling ---
typedef struct {
    char date[11];
    int hour;
    double watts;
} storage_solar_row_t;

int storage_write_solar_forecast(const storage_solar_row_t * rows, int n);
void storage_solar_forecast_curve(ems_date_t date, opt_d out[24]);                     // kW
void storage_solar_forecast_curve_quarterly(ems_date_t date, opt_d out[QUARTERS_PER_DAY]); // kW

// --- battery_log ---
void storage_log_battery_event(const char * event, const cJSON * detail); // detail mag NULL
int storage_prune_battery_log(int retention_days);

// --- opschonen ruwe metingen (ESP32-specifiek) ---
int storage_prune_measurements(int retention_days);

// --- CSV-exports: `write` wordt per stuk tekst aangeroepen (bv. naar HTTP) ---
typedef void (*storage_write_fn)(const char * text, void * ctx);
int storage_export_consumption_csv(storage_write_fn write, void * ctx);
int storage_export_battery_log_csv(storage_write_fn write, void * ctx);

// --- import van de Pi-database (eenmalig) ---
// Kopieert consumption_quarterly, savings_quarterly, battery_pool_state en
// solar_forecast uit een losse SQLite-file (de Pi-export) in de eigen DB.
bool storage_import_from_db(const char * import_db_path, cJSON * report);

// Port van dashboard-ui/savings_reset_writer.py: wist savings_quarterly en
// battery_pool_state (alle tellers terug naar 0) plus de nog niet
// weggeschreven buffer. false + reden bij een echte fout.
bool storage_reset_savings(char * error_out, int error_out_size);

#ifdef __cplusplus
}
#endif
