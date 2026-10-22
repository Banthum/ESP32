#include <Arduino.h>

#include "storage.h"

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "../core/log.h"
#include "../core/paths.h"

static const char * TAG = "dashboard.storage";

static sqlite3 * s_db = nullptr;
static SemaphoreHandle_t s_mutex = nullptr;

struct db_lock {
    db_lock() { xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY); }
    ~db_lock() { xSemaphoreGiveRecursive(s_mutex); }
};

// Lange queries (verbruikscurve over weken historie) lezen duizenden pagina's
// van flash; zonder pauze kreeg de idle-taak van core 0 >5 s geen CPU en
// greep de task-watchdog in. Elke ~50 ms query-tijd even 1 tick afstaan.
static int yield_progress(void * arg)
{
    (void) arg;
    static uint32_t s_last_yield = 0;
    uint32_t now = millis();
    if(now - s_last_yield >= 50) {
        vTaskDelay(1);
        s_last_yield = millis();
    }
    return 0;
}

static bool exec(const char * sql)
{
    char * err = nullptr;
    if(sqlite3_exec(s_db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        BMS_LOGW(TAG, "SQL-fout: %s", err ? err : "?");
        sqlite3_free(err);
        return false;
    }
    return true;
}

static sqlite3_stmt * prepare(const char * sql)
{
    sqlite3_stmt * st = nullptr;
    if(sqlite3_prepare_v2(s_db, sql, -1, &st, nullptr) != SQLITE_OK) {
        BMS_LOGW(TAG, "prepare mislukt: %s (%.60s)", sqlite3_errmsg(s_db), sql);
        return nullptr;
    }
    return st;
}

static const char * SCHEMA = R"SQL(
    CREATE TABLE IF NOT EXISTS measurements (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        timestamp INTEGER NOT NULL,
        source TEXT NOT NULL,
        value REAL NOT NULL,
        unit TEXT
    );
    CREATE INDEX IF NOT EXISTS idx_measurements_source_ts
        ON measurements(source, timestamp);

    CREATE TABLE IF NOT EXISTS price_curve (
        date TEXT NOT NULL,
        hour INTEGER NOT NULL,
        price_eur_kwh REAL NOT NULL,
        PRIMARY KEY (date, hour)
    );

    CREATE TABLE IF NOT EXISTS solar_forecast (
        date TEXT NOT NULL,
        hour INTEGER NOT NULL,
        forecast_w REAL NOT NULL,
        PRIMARY KEY (date, hour)
    );

    CREATE TABLE IF NOT EXISTS consumption_quarterly (
        date TEXT NOT NULL,
        quarter_of_day INTEGER NOT NULL,
        avg_power_w REAL NOT NULL,
        is_weekend INTEGER NOT NULL,
        PRIMARY KEY (date, quarter_of_day)
    );

    CREATE TABLE IF NOT EXISTS battery_log (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        timestamp INTEGER NOT NULL,
        event TEXT NOT NULL,
        detail TEXT
    );
    CREATE INDEX IF NOT EXISTS idx_battery_log_ts ON battery_log(timestamp);

    CREATE TABLE IF NOT EXISTS savings_quarterly (
        date TEXT NOT NULL,
        quarter_of_day INTEGER NOT NULL,
        batterij_eur REAL NOT NULL,
        ems_eur REAL NOT NULL,
        PRIMARY KEY (date, quarter_of_day)
    );

    CREATE TABLE IF NOT EXISTS battery_pool_state (
        id INTEGER PRIMARY KEY CHECK (id = 1),
        zon_kwh REAL NOT NULL
    );
)SQL";

// SQLite doet veel kleine allocaties; standaard belanden die (<4 KB) in het
// schaarse interne RAM dat WiFi/TLS/LVGL nodig hebben. Alles naar PSRAM.
// De gevraagde grootte staat in een eigen header van 8 bytes vóór elk blok:
// heap_caps_get_allocated_size() geeft (met heap-poisoning) méér terug dan
// bruikbaar is, en SQLite gebruikt xSize() echt tot de laatste byte - dat gaf
// "CORRUPT HEAP: Bad tail".
static const int SQ_HDR = 8;
static void * sq_malloc(int n)
{
    uint8_t * p = (uint8_t *) heap_caps_malloc(n + SQ_HDR, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!p) return nullptr;
    *(int *) p = n;
    return p + SQ_HDR;
}
static void sq_free(void * p)
{
    if(p) heap_caps_free((uint8_t *) p - SQ_HDR);
}
static void * sq_realloc(void * p, int n)
{
    if(!p) return sq_malloc(n);
    uint8_t * base = (uint8_t *) heap_caps_realloc((uint8_t *) p - SQ_HDR, n + SQ_HDR, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!base) return nullptr;
    *(int *) base = n;
    return base + SQ_HDR;
}
static int sq_size(void * p)
{
    return p ? *(int *) ((uint8_t *) p - SQ_HDR) : 0;
}
static int sq_roundup(int n)
{
    return (n + 7) & ~7;
}
static int sq_init(void *)
{
    return SQLITE_OK;
}
static void sq_shutdown(void *) {}

// De database draait in PSRAM (":memory:") en wordt periodiek in zijn geheel
// naar LittleFS geschreven. Rechtstreeks op LittleFS (copy-on-write) liet elke
// kleine write een groot deel van het 550 KB-bestand herschrijven: 5 rijen
// wegschrijven kostte 45 s aan flash-erases, en tijdens elke erase ligt PSRAM
// stil - de RGB-DMA liep dan leeg en het beeld sprong continu. Nu is er alleen
// flash-activiteit bij het laden (boot) en bij storage_persist().
static char s_db_path[96];
static char s_db_tmp_path[100];
static uint32_t s_last_persist_ms = 0;

// Kopieert de hele database van src naar dst (SQLite online-backup), in
// stukjes met een korte pauze zodat andere taken (en de idle-taak) door kunnen.
static bool backup_db(sqlite3 * dst, sqlite3 * src)
{
    sqlite3_backup * b = sqlite3_backup_init(dst, "main", src, "main");
    if(!b) {
        BMS_LOGW(TAG, "backup starten mislukt: %s", sqlite3_errmsg(dst));
        return false;
    }
    int rc;
    do {
        rc = sqlite3_backup_step(b, 16);
        vTaskDelay(1);
    } while(rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED);
    sqlite3_backup_finish(b);
    return rc == SQLITE_DONE;
}

// De oude opzet (database direct op LittleFS) liet tijdelijke SQLite-
// bestanden ("tmp_t...") in de root achter die niet verwijderd konden worden.
static void remove_stale_temp_files(void)
{
    DIR * dir = opendir("/littlefs");
    if(!dir) return;
    struct dirent * e;
    char path[300];
    while((e = readdir(dir)) != nullptr) {
        if(strncmp(e->d_name, "tmp_t", 5) == 0) {
            snprintf(path, sizeof(path), "/littlefs/%s", e->d_name);
            if(remove(path) == 0) BMS_LOGI(TAG, "oud tempbestand verwijderd: %s", path);
        }
    }
    closedir(dir);
}

static void load_from_file(void)
{
    FILE * f = fopen(s_db_path, "rb");
    if(!f) {
        BMS_LOGI(TAG, "nog geen database op flash (%s), begint leeg", s_db_path);
        return;
    }
    fclose(f);
    sqlite3 * file_db = nullptr;
    uint32_t t0 = millis();
    // Lezen/schrijven i.p.v. read-only: een achtergebleven journal (oude versie
    // schreef direct op flash) moet SQLite eerst kunnen terugdraaien.
    if(sqlite3_open(s_db_path, &file_db) == SQLITE_OK &&
       backup_db(s_db, file_db)) {
        BMS_LOGI(TAG, "database geladen in %lu ms", (unsigned long) (millis() - t0));
    } else {
        BMS_LOGE(TAG, "database laden mislukt: %s", file_db ? sqlite3_errmsg(file_db) : "?");
    }
    sqlite3_close(file_db);
}

bool storage_persist(void)
{
    db_lock lock;
    if(!s_db) {
        return false;
    }
    uint32_t t0 = millis();
    remove(s_db_tmp_path);
    sqlite3 * file_db = nullptr;
    bool ok = sqlite3_open(s_db_tmp_path, &file_db) == SQLITE_OK;
    if(ok) {
        // Geen journal: het tmp-bestand vervangt het origineel pas na een
        // volledige, gesloten backup (rename is atomair op LittleFS).
        sqlite3_exec(file_db, "PRAGMA journal_mode=OFF", nullptr, nullptr, nullptr);
        ok = backup_db(file_db, s_db);
    }
    sqlite3_close(file_db);
    if(ok) {
        ok = rename(s_db_tmp_path, s_db_path) == 0;
    }
    s_last_persist_ms = millis();
    if(ok) {
        struct stat st;
        long size = stat(s_db_path, &st) == 0 ? (long) st.st_size : -1;
        BMS_LOGI(TAG, "database weggeschreven (%ld bytes) in %lu ms", size, (unsigned long) (millis() - t0));
    } else {
        BMS_LOGW(TAG, "database wegschrijven mislukt");
        remove(s_db_tmp_path);
    }
    return ok;
}

void storage_persist_if_due(void)
{
    if(millis() - s_last_persist_ms >= STORAGE_PERSIST_INTERVAL_MS) {
        storage_persist();
    }
}

bool storage_init(const char * db_path)
{
    if(!s_mutex) {
        s_mutex = xSemaphoreCreateRecursiveMutex();
    }
    db_lock lock;
    static const sqlite3_mem_methods psram_methods = {sq_malloc, sq_free,     sq_realloc, sq_size,
                                                      sq_roundup, sq_init, sq_shutdown, nullptr};
    if(sqlite3_config(SQLITE_CONFIG_MALLOC, &psram_methods) != SQLITE_OK) {
        BMS_LOGW(TAG, "kon SQLite niet naar PSRAM laten alloceren");
    }
    sqlite3_initialize();
    strlcpy(s_db_path, db_path, sizeof(s_db_path));
    snprintf(s_db_tmp_path, sizeof(s_db_tmp_path), "%s.tmp", db_path);
    if(sqlite3_open(":memory:", &s_db) != SQLITE_OK) {
        BMS_LOGE(TAG, "kan geen database in geheugen openen: %s", sqlite3_errmsg(s_db));
        sqlite3_close(s_db);
        s_db = nullptr;
        return false;
    }
    sqlite3_progress_handler(s_db, 1000, yield_progress, nullptr);
    exec("PRAGMA temp_store=MEMORY");
    remove_stale_temp_files();
    load_from_file();
    s_last_persist_ms = millis();
    if(!exec(SCHEMA)) {
        return false;
    }
    // ESP32-toevoeging (niet in storage.py): index voor expected_consumption()'s
    // WHERE quarter_of_day/is_weekend/date - 192 query's per dagcurve.
    // Verandert geen enkele uitkomst.
    exec("CREATE INDEX IF NOT EXISTS idx_consumption_quarter_weekend_date "
         "ON consumption_quarterly(quarter_of_day, is_weekend, date)");
    BMS_LOGI(TAG, "database geopend (in PSRAM, bron %s)", db_path);
    return true;
}

void storage_close(void)
{
    db_lock lock;
    if(s_db) {
        storage_persist();
        sqlite3_close(s_db);
        s_db = nullptr;
    }
}

// ---------------------------------------------------------------- buffers

#define MAX_SOURCES 8
typedef struct {
    char source[24];
    char unit[8];
    double sum;
    int count;
} sample_acc_t;
static sample_acc_t s_samples[MAX_SOURCES];
static int s_sample_sources = 0;

void storage_measurement_record(const char * source, double value, const char * unit)
{
    db_lock lock;
    for(int i = 0; i < s_sample_sources; i++) {
        if(strcmp(s_samples[i].source, source) == 0) {
            s_samples[i].sum += value;
            s_samples[i].count++;
            snprintf(s_samples[i].unit, sizeof(s_samples[i].unit), "%s", unit ? unit : "W");
            return;
        }
    }
    if(s_sample_sources == MAX_SOURCES) {
        return;
    }
    sample_acc_t * a = &s_samples[s_sample_sources++];
    snprintf(a->source, sizeof(a->source), "%s", source);
    snprintf(a->unit, sizeof(a->unit), "%s", unit ? unit : "W");
    a->sum = value;
    a->count = 1;
}

void storage_measurement_flush(void)
{
    db_lock lock;
    if(s_sample_sources == 0 || !s_db) {
        return;
    }
    long now = (long) time(nullptr);
    exec("BEGIN");
    sqlite3_stmt * st = prepare("INSERT INTO measurements (timestamp, source, value, unit) VALUES (?, ?, ?, ?)");
    for(int i = 0; st && i < s_sample_sources; i++) {
        sqlite3_bind_int64(st, 1, now);
        sqlite3_bind_text(st, 2, s_samples[i].source, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(st, 3, s_samples[i].sum / s_samples[i].count);
        sqlite3_bind_text(st, 4, s_samples[i].unit, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    exec("COMMIT");
    BMS_LOGI(TAG, "Flushed %d averaged measurement(s) to storage", s_sample_sources);
    s_sample_sources = 0;
}

static double s_savings_batterij = 0.0;
static double s_savings_ems = 0.0;

void storage_savings_record(double batterij_eur, double ems_eur)
{
    db_lock lock;
    s_savings_batterij += batterij_eur;
    s_savings_ems += ems_eur;
}

void storage_savings_pending(double * batterij_eur, double * ems_eur)
{
    db_lock lock;
    *batterij_eur = s_savings_batterij;
    *ems_eur = s_savings_ems;
}

void storage_savings_flush(void)
{
    db_lock lock;
    if((s_savings_batterij == 0.0 && s_savings_ems == 0.0) || !s_db) {
        return;
    }
    time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    char date[11];
    strftime(date, sizeof(date), "%Y-%m-%d", &local);
    int quarter = (local.tm_hour * 60 + local.tm_min) / 15;

    // Geen "ON CONFLICT ... DO UPDATE": Sqlite3Esp32 bouwt SQLite met
    // YYSTACKDEPTH 20 en daarvoor is de parser-stack dan te klein. Dus eerst
    // de rij aanmaken (als hij nog niet bestaat) en daarna optellen.
    bool ok = false;
    exec("BEGIN");
    sqlite3_stmt * st = prepare("INSERT OR IGNORE INTO savings_quarterly (date, quarter_of_day, batterij_eur, ems_eur) "
                                "VALUES (?, ?, 0, 0)");
    if(st) {
        sqlite3_bind_text(st, 1, date, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, quarter);
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
    }
    st = ok ? prepare("UPDATE savings_quarterly SET batterij_eur = batterij_eur + ?, ems_eur = ems_eur + ? "
                      "WHERE date = ? AND quarter_of_day = ?")
            : nullptr;
    ok = false;
    if(st) {
        sqlite3_bind_double(st, 1, s_savings_batterij);
        sqlite3_bind_double(st, 2, s_savings_ems);
        sqlite3_bind_text(st, 3, date, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 4, quarter);
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
    }
    exec(ok ? "COMMIT" : "ROLLBACK");
    if(!ok) {
        // Bedragen bewaren voor de volgende flush i.p.v. ze kwijt te raken.
        BMS_LOGW(TAG, "Flush savings mislukt: %s", sqlite3_errmsg(s_db));
        return;
    }
    BMS_LOGI(TAG, "Flushed savings: batterij=%.4f ems=%.4f EUR", s_savings_batterij, s_savings_ems);
    s_savings_batterij = 0.0;
    s_savings_ems = 0.0;
}

// ---------------------------------------------------------------- pool state

double storage_load_pool_state(void)
{
    db_lock lock;
    double v = 0.0;
    sqlite3_stmt * st = prepare("SELECT zon_kwh FROM battery_pool_state WHERE id = 1");
    if(st && sqlite3_step(st) == SQLITE_ROW) {
        v = sqlite3_column_double(st, 0);
    }
    sqlite3_finalize(st);
    return v;
}

void storage_save_pool_state(double shadow_soc_kwh)
{
    db_lock lock;
    sqlite3_stmt * st = prepare("INSERT OR REPLACE INTO battery_pool_state (id, zon_kwh) VALUES (1, ?)");
    if(st) {
        sqlite3_bind_double(st, 1, shadow_soc_kwh);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
}

// ---------------------------------------------------------------- savings totals

static savings_pair_t savings_total(const char * start_date, const char * end_date)
{
    savings_pair_t out = {0.0, 0.0};
    sqlite3_stmt * st;
    if(start_date == nullptr) {
        st = prepare("SELECT COALESCE(SUM(batterij_eur),0), COALESCE(SUM(ems_eur),0) "
                     "FROM savings_quarterly WHERE date <= ?");
        if(st) sqlite3_bind_text(st, 1, end_date, -1, SQLITE_TRANSIENT);
    } else {
        st = prepare("SELECT COALESCE(SUM(batterij_eur),0), COALESCE(SUM(ems_eur),0) "
                     "FROM savings_quarterly WHERE date >= ? AND date <= ?");
        if(st) {
            sqlite3_bind_text(st, 1, start_date, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, end_date, -1, SQLITE_TRANSIENT);
        }
    }
    if(st && sqlite3_step(st) == SQLITE_ROW) {
        out.batterij_eur = sqlite3_column_double(st, 0);
        out.ems_eur = sqlite3_column_double(st, 1);
    }
    sqlite3_finalize(st);
    return out;
}

savings_summary_t storage_savings_summary(ems_date_t today)
{
    db_lock lock;
    char today_s[11], week_s[11], month_s[11], year_s[11];
    date_iso(today, today_s);
    date_iso(date_add_days(today, -date_weekday(today)), week_s);
    date_iso({today.year, today.month, 1}, month_s);
    date_iso({today.year, 1, 1}, year_s);

    savings_summary_t s;
    s.dag = savings_total(today_s, today_s);
    s.week = savings_total(week_s, today_s);
    s.maand = savings_total(month_s, today_s);
    s.jaar = savings_total(year_s, today_s);
    s.altijd = savings_total(nullptr, today_s);
    return s;
}

// ---------------------------------------------------------------- aggregatie

static const char * QUARTER_SQL =
    "SELECT ((CAST(strftime('%H', timestamp, 'unixepoch', 'localtime') AS INTEGER) * 60 "
    "  + CAST(strftime('%M', timestamp, 'unixepoch', 'localtime') AS INTEGER)) / 15) AS quarter, "
    "  AVG(value) AS avg_power_w "
    "FROM measurements "
    "WHERE source = ? AND date(timestamp, 'unixepoch', 'localtime') = ? "
    "GROUP BY quarter";

int storage_aggregate_quarterly(const char * date_str, const char * source)
{
    db_lock lock;
    double avg[QUARTERS_PER_DAY];
    bool have[QUARTERS_PER_DAY] = {};
    int n = 0;
    sqlite3_stmt * st = prepare(QUARTER_SQL);
    if(!st) {
        return 0;
    }
    sqlite3_bind_text(st, 1, source, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, date_str, -1, SQLITE_TRANSIENT);
    while(sqlite3_step(st) == SQLITE_ROW) {
        int q = sqlite3_column_int(st, 0);
        if(q >= 0 && q < QUARTERS_PER_DAY) {
            avg[q] = sqlite3_column_double(st, 1);
            have[q] = true;
            n++;
        }
    }
    sqlite3_finalize(st);

    if(n == 0) {
        BMS_LOGI(TAG, "No %s measurements for %s, nothing to aggregate", source, date_str);
        return 0;
    }

    ems_date_t d;
    date_parse_iso(date_str, &d);
    int is_weekend = date_weekday(d) >= 5 ? 1 : 0;

    exec("BEGIN");
    st = prepare("INSERT OR REPLACE INTO consumption_quarterly (date, quarter_of_day, avg_power_w, is_weekend) "
                 "VALUES (?, ?, ?, ?)");
    for(int q = 0; st && q < QUARTERS_PER_DAY; q++) {
        if(!have[q]) continue;
        sqlite3_bind_text(st, 1, date_str, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, q);
        sqlite3_bind_double(st, 3, avg[q]);
        sqlite3_bind_int(st, 4, is_weekend);
        sqlite3_step(st);
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    exec("COMMIT");
    BMS_LOGI(TAG, "Aggregated %d quarter-hour bucket(s) for %s (source=%s)", n, date_str, source);
    return n;
}

void storage_actual_consumption_curve(const char * source, ems_date_t date, opt_d out[QUARTERS_PER_DAY])
{
    db_lock lock;
    opt_fill_none(out, QUARTERS_PER_DAY);
    char date_s[11];
    date_iso(date, date_s);
    sqlite3_stmt * st = prepare(QUARTER_SQL);
    if(!st) {
        return;
    }
    sqlite3_bind_text(st, 1, source, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, date_s, -1, SQLITE_TRANSIENT);
    while(sqlite3_step(st) == SQLITE_ROW) {
        int q = sqlite3_column_int(st, 0);
        if(q >= 0 && q < QUARTERS_PER_DAY) {
            out[q] = opt_some(sqlite3_column_double(st, 1) * 0.25 / 1000);
        }
    }
    sqlite3_finalize(st);
}

bool storage_overnight_baseline_load_w(const char * date_str, double * out_w)
{
    db_lock lock;
    sqlite3_stmt * st = prepare("SELECT avg_power_w FROM consumption_quarterly "
                                "WHERE date = ? AND quarter_of_day >= ? AND quarter_of_day < ?");
    if(!st) {
        return false;
    }
    sqlite3_bind_text(st, 1, date_str, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, OVERNIGHT_BASELINE_START_QUARTER);
    sqlite3_bind_int(st, 3, OVERNIGHT_BASELINE_END_QUARTER);
    double sum = 0;
    int n = 0;
    while(sqlite3_step(st) == SQLITE_ROW) {
        sum += sqlite3_column_double(st, 0);
        n++;
    }
    sqlite3_finalize(st);
    if(n == 0) {
        return false;
    }
    *out_w = sum / n;
    return true;
}

// ---------------------------------------------------------------- voorspelling

void storage_load_default_consumption_profile(const char * path, opt_d profile[QUARTERS_PER_DAY])
{
    opt_fill_none(profile, QUARTERS_PER_DAY);
    FILE * f = fopen(path, "r");
    if(!f) {
        return;
    }
    char line[160];
    int tijd_col = -1, vermogen_col = -1;
    bool header = true;
    while(fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        // ;-gescheiden, Nederlandse komma-decimalen (csv.DictReader op de Pi).
        char * fields[8];
        int nf = 0;
        for(char * tok = line; tok && nf < 8;) {
            fields[nf++] = tok;
            char * semi = strchr(tok, ';');
            if(semi) {
                *semi = '\0';
                tok = semi + 1;
            } else {
                tok = nullptr;
            }
        }
        if(header) {
            for(int i = 0; i < nf; i++) {
                // utf-8 BOM vóór de eerste kolomnaam overslaan
                const char * name = fields[i];
                if((unsigned char) name[0] == 0xEF) name += 3;
                if(strcmp(name, "tijd") == 0) tijd_col = i;
                if(strcmp(name, "gemiddeld_vermogen_w") == 0) vermogen_col = i;
            }
            header = false;
            continue;
        }
        if(tijd_col < 0 || vermogen_col < 0 || tijd_col >= nf || vermogen_col >= nf) {
            continue;
        }
        int h, m;
        if(sscanf(fields[tijd_col], "%d:%d", &h, &m) != 2) {
            continue;
        }
        for(char * c = fields[vermogen_col]; *c; c++) {
            if(*c == ',') *c = '.';
        }
        int q = (h * 60 + m) / 15;
        if(q >= 0 && q < QUARTERS_PER_DAY) {
            profile[q] = opt_some(atof(fields[vermogen_col]));
        }
    }
    fclose(f);
}

opt_d storage_expected_consumption(time_t timestamp, int lookback_days, int min_samples, const opt_d * profile)
{
    db_lock lock;
    struct tm local;
    localtime_r(&timestamp, &local);
    int quarter = (local.tm_hour * 60 + local.tm_min) / 15;
    ems_date_t d = {local.tm_year + 1900, local.tm_mon + 1, local.tm_mday};
    int is_weekend = date_weekday(d) >= 5 ? 1 : 0;

    char end_s[11], start_s[11];
    date_iso(date_add_days(d, -1), end_s);
    date_iso(date_add_days(d, -lookback_days), start_s);

    sqlite3_stmt * st = prepare("SELECT avg_power_w FROM consumption_quarterly "
                                "WHERE quarter_of_day = ? AND is_weekend = ? AND date >= ? AND date <= ? "
                                "ORDER BY date ASC");
    if(!st) {
        return opt_none();
    }
    sqlite3_bind_int(st, 1, quarter);
    sqlite3_bind_int(st, 2, is_weekend);
    sqlite3_bind_text(st, 3, start_s, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, end_s, -1, SQLITE_TRANSIENT);
    double rows[64];
    int n = 0;
    while(sqlite3_step(st) == SQLITE_ROW && n < 64) {
        rows[n++] = sqlite3_column_double(st, 0);
    }
    sqlite3_finalize(st);

    if(n < min_samples) {
        if(profile && profile[quarter].valid) {
            return profile[quarter];
        }
        return opt_none();
    }

    // Lineair oplopend gewicht (oudste 1 ... nieuwste n); negatieve rijen
    // (oude "grid_power"-data) afgekapt op 0 - zie storage.py.
    double weighted = 0;
    for(int i = 0; i < n; i++) {
        double v = rows[i] > 0 ? rows[i] : 0.0;
        weighted += (i + 1) * v;
    }
    double weight_sum = n * (n + 1) / 2.0;
    return opt_some(weighted / weight_sum);
}

// Zelfde uitkomst als 96x storage_expected_consumption() (zoals
// expected_consumption_curve() op de Pi), maar met één query: weekend-vlag en
// datumvenster zijn voor alle kwartieren van één dag gelijk. Op flash kost
// elke losse query ~65 ms, 96 per dag was merkbaar traag.
void storage_expected_consumption_curve(ems_date_t date, opt_d out[QUARTERS_PER_DAY])
{
    static opt_d profile[QUARTERS_PER_DAY];
    storage_load_default_consumption_profile(BMS_DEFAULT_CONSUMPTION_PROFILE_PATH, profile);

    const int lookback_days = 14;
    const int min_samples = 1;
    int is_weekend = date_weekday(date) >= 5 ? 1 : 0;
    char end_s[11], start_s[11];
    date_iso(date_add_days(date, -1), end_s);
    date_iso(date_add_days(date, -lookback_days), start_s);

    // Per kwartier: oplopend naar datum gesorteerde rijen -> gewogen som.
    static double weighted[QUARTERS_PER_DAY];
    static int count[QUARTERS_PER_DAY];
    for(int q = 0; q < QUARTERS_PER_DAY; q++) {
        weighted[q] = 0.0;
        count[q] = 0;
    }
    {
        db_lock lock;
        sqlite3_stmt * st = prepare("SELECT quarter_of_day, avg_power_w FROM consumption_quarterly "
                                    "WHERE is_weekend = ? AND date >= ? AND date <= ? "
                                    "ORDER BY date ASC");
        if(st) {
            sqlite3_bind_int(st, 1, is_weekend);
            sqlite3_bind_text(st, 2, start_s, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 3, end_s, -1, SQLITE_TRANSIENT);
            while(sqlite3_step(st) == SQLITE_ROW) {
                int q = sqlite3_column_int(st, 0);
                if(q < 0 || q >= QUARTERS_PER_DAY) continue;
                double v = sqlite3_column_double(st, 1);
                count[q]++;
                weighted[q] += count[q] * (v > 0 ? v : 0.0); // gewicht 1..n, oudste eerst
            }
            sqlite3_finalize(st);
        }
    }

    for(int q = 0; q < QUARTERS_PER_DAY; q++) {
        int n = count[q];
        if(n < min_samples) {
            out[q] = profile[q].valid ? opt_some(profile[q].v * 0.25 / 1000) : opt_none();
            continue;
        }
        double w = weighted[q] / (n * (n + 1) / 2.0);
        out[q] = opt_some(w * 0.25 / 1000);
    }
}

// ---------------------------------------------------------------- zon

int storage_write_solar_forecast(const storage_solar_row_t * rows, int n)
{
    db_lock lock;
    if(n <= 0) {
        return 0;
    }
    exec("BEGIN");
    sqlite3_stmt * st = prepare("INSERT OR REPLACE INTO solar_forecast (date, hour, forecast_w) VALUES (?, ?, ?)");
    for(int i = 0; st && i < n; i++) {
        sqlite3_bind_text(st, 1, rows[i].date, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, rows[i].hour);
        sqlite3_bind_double(st, 3, rows[i].watts);
        sqlite3_step(st);
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    exec("COMMIT");
    BMS_LOGI(TAG, "Wrote %d solar_forecast row(s)", n);
    return n;
}

static void solar_hourly_w(const char * date_s, opt_d out[24])
{
    opt_fill_none(out, 24);
    sqlite3_stmt * st = prepare("SELECT hour, forecast_w FROM solar_forecast WHERE date = ?");
    if(!st) {
        return;
    }
    sqlite3_bind_text(st, 1, date_s, -1, SQLITE_TRANSIENT);
    while(sqlite3_step(st) == SQLITE_ROW) {
        int h = sqlite3_column_int(st, 0);
        if(h >= 0 && h < 24) {
            out[h] = opt_some(sqlite3_column_double(st, 1));
        }
    }
    sqlite3_finalize(st);
}

void storage_solar_forecast_curve(ems_date_t date, opt_d out[24])
{
    db_lock lock;
    char date_s[11];
    date_iso(date, date_s);
    solar_hourly_w(date_s, out);
    for(int h = 0; h < 24; h++) {
        if(out[h].valid) out[h].v /= 1000;
    }
}

void storage_solar_forecast_curve_quarterly(ems_date_t date, opt_d out[QUARTERS_PER_DAY])
{
    db_lock lock;
    opt_d hourly[24], tomorrow[24];
    storage_solar_forecast_curve(date, hourly);
    char tomorrow_s[11];
    date_iso(date_add_days(date, 1), tomorrow_s);
    solar_hourly_w(tomorrow_s, tomorrow);
    opt_d next_day_hour0 = tomorrow[0].valid ? opt_some(tomorrow[0].v / 1000) : opt_none();

    opt_fill_none(out, QUARTERS_PER_DAY);
    for(int h = 0; h < 24; h++) {
        opt_d start = hourly[h];
        opt_d end = h + 1 < 24 ? hourly[h + 1] : next_day_hour0;
        for(int k = 0; k < 4; k++) {
            int idx = h * 4 + k;
            if(!start.valid) continue;
            out[idx] = end.valid ? opt_some(start.v + (end.v - start.v) * k / 4) : start;
        }
    }
}

// ---------------------------------------------------------------- battery_log

void storage_log_battery_event(const char * event, const cJSON * detail)
{
    db_lock lock;
    char * json = detail ? cJSON_PrintUnformatted(detail) : nullptr;
    sqlite3_stmt * st = prepare("INSERT INTO battery_log (timestamp, event, detail) VALUES (?, ?, ?)");
    if(st) {
        sqlite3_bind_int64(st, 1, (long) time(nullptr));
        sqlite3_bind_text(st, 2, event, -1, SQLITE_TRANSIENT);
        if(json) sqlite3_bind_text(st, 3, json, -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(st, 3);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    free(json);
}

static int delete_older_than(const char * sql, int retention_days)
{
    sqlite3_stmt * st = prepare(sql);
    if(!st) {
        return 0;
    }
    sqlite3_bind_int64(st, 1, (long) time(nullptr) - (long) retention_days * 86400);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return sqlite3_changes(s_db);
}

int storage_prune_battery_log(int retention_days)
{
    db_lock lock;
    int n = delete_older_than("DELETE FROM battery_log WHERE timestamp < ?", retention_days);
    BMS_LOGI(TAG, "Pruned %d battery_log row(s) older than %d days", n, retention_days);
    return n;
}

int storage_prune_measurements(int retention_days)
{
    db_lock lock;
    int n = delete_older_than("DELETE FROM measurements WHERE timestamp < ?", retention_days);
    if(n > 0) {
        // Vrijgekomen pagina's teruggeven aan het bestandssysteem.
        exec("VACUUM");
    }
    BMS_LOGI(TAG, "Pruned %d measurement row(s) older than %d days", n, retention_days);
    return n;
}

// ---------------------------------------------------------------- CSV

int storage_export_consumption_csv(storage_write_fn write, void * ctx)
{
    db_lock lock;
    write("date,quarter_of_day,time_of_day,avg_power_w,is_weekend\r\n", ctx);
    sqlite3_stmt * st = prepare("SELECT date, quarter_of_day, avg_power_w, is_weekend FROM consumption_quarterly "
                                "ORDER BY date ASC, quarter_of_day ASC");
    int n = 0;
    char line[96];
    while(st && sqlite3_step(st) == SQLITE_ROW) {
        int q = sqlite3_column_int(st, 1);
        snprintf(line, sizeof(line), "%s,%d,%02d:%02d,%.17g,%d\r\n", sqlite3_column_text(st, 0), q, (q * 15) / 60,
                 (q * 15) % 60, sqlite3_column_double(st, 2), sqlite3_column_int(st, 3));
        write(line, ctx);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

int storage_export_battery_log_csv(storage_write_fn write, void * ctx)
{
    db_lock lock;
    write("timestamp_utc,event,detail\r\n", ctx);
    sqlite3_stmt * st = prepare("SELECT timestamp, event, detail FROM battery_log ORDER BY timestamp ASC");
    int n = 0;
    while(st && sqlite3_step(st) == SQLITE_ROW) {
        char iso[32];
        iso_utc((time_t) sqlite3_column_int64(st, 0), iso, sizeof(iso));
        const char * detail = (const char *) sqlite3_column_text(st, 2);
        // detail is JSON met komma's/aanhalingstekens -> CSV-quoten zoals csv.writer
        String q = "\"";
        for(const char * c = detail ? detail : ""; *c; c++) {
            if(*c == '"') q += '"';
            q += *c;
        }
        q += "\"";
        String line = String(iso) + "," + (const char *) sqlite3_column_text(st, 1) + "," + q + "\r\n";
        write(line.c_str(), ctx);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

// ---------------------------------------------------------------- reset

bool storage_reset_savings(char * error_out, int error_out_size)
{
    db_lock lock;
    char * err = nullptr;
    int rc = sqlite3_exec(s_db, "BEGIN; DELETE FROM savings_quarterly; DELETE FROM battery_pool_state; COMMIT;",
                          nullptr, nullptr, &err);
    if(rc != SQLITE_OK) {
        snprintf(error_out, error_out_size, "kon besparingsdata niet resetten: %s", err ? err : "?");
        sqlite3_free(err);
        exec("ROLLBACK");
        return false;
    }
    s_savings_batterij = 0.0;
    s_savings_ems = 0.0;
    BMS_LOGI(TAG, "besparingstellers gereset");
    storage_persist();
    return true;
}

// ---------------------------------------------------------------- import

bool storage_import_from_db(const char * import_db_path, cJSON * report)
{
    db_lock lock;
    char sql[256];
    snprintf(sql, sizeof(sql), "ATTACH DATABASE '%s' AS pi", import_db_path);
    if(!exec(sql)) {
        return false;
    }
    struct {
        const char * table;
        const char * insert_sql;
    } steps[] = {
        {"consumption_quarterly", "INSERT OR REPLACE INTO consumption_quarterly (date, quarter_of_day, avg_power_w, "
                                  "is_weekend) SELECT date, quarter_of_day, avg_power_w, is_weekend FROM "
                                  "pi.consumption_quarterly"},
        {"savings_quarterly", "INSERT OR REPLACE INTO savings_quarterly (date, quarter_of_day, batterij_eur, ems_eur) "
                              "SELECT date, quarter_of_day, batterij_eur, ems_eur FROM pi.savings_quarterly"},
        {"solar_forecast", "INSERT OR REPLACE INTO solar_forecast (date, hour, forecast_w) "
                           "SELECT date, hour, forecast_w FROM pi.solar_forecast"},
        {"battery_pool_state", "INSERT OR REPLACE INTO battery_pool_state (id, zon_kwh) "
                               "SELECT id, zon_kwh FROM pi.battery_pool_state WHERE id = 1"},
    };
    bool ok = exec("BEGIN");
    for(auto & s : steps) {
        bool step_ok = exec(s.insert_sql);
        int rows = step_ok ? sqlite3_changes(s_db) : -1;
        if(report) cJSON_AddNumberToObject(report, s.table, rows);
        ok = ok && step_ok;
    }
    exec(ok ? "COMMIT" : "ROLLBACK");
    exec("DETACH DATABASE pi");
    BMS_LOGI(TAG, "import uit %s: %s", import_db_path, ok ? "gelukt" : "MISLUKT (teruggedraaid)");
    if(ok) storage_persist();
    return ok;
}
