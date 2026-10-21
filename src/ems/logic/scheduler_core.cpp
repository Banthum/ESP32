#include "scheduler_core.h"

#include <esp_heap_caps.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static sched_yield_fn s_yield = nullptr;

void sched_set_yield_hook(sched_yield_fn fn)
{
    s_yield = fn;
}

static double dmin(double a, double b)
{
    return a < b ? a : b;
}

static double dmax(double a, double b)
{
    return a > b ? a : b;
}

// Python's round(x, 4) (round-half-even op de decimale weergave); nearbyint
// gebruikt de standaard afrondmodus (half-even) - in de praktijk identiek.
static double round4(double x)
{
    return nearbyint(x * 10000.0) / 10000.0;
}

sched_battery_t sched_battery_default(void)
{
    sched_battery_t b;
    b.capacity_kwh = 5.12;
    b.max_charge_kwh_per_interval = 0.625;
    b.max_discharge_kwh_per_interval = 0.625;
    b.min_reserve_kwh = 0.0;
    b.degradation_cost_per_kwh = 0.0195;
    b.round_trip_efficiency = 0.95;
    b.has_max_soc = false;
    b.max_soc_kwh = 0.0;
    return b;
}

void sched_simulate_plan(int n, const double * import_prices, const double * export_prices, const double * load_kwh,
                         const double * solar_kwh, const double * charge_plan, const double * discharge_plan,
                         double soc_start_kwh, const sched_battery_t * battery, sched_sim_result_t * out)
{
    double soc = soc_start_kwh;
    double total_cost = 0.0, total_degradation = 0.0, total_export_revenue = 0.0;
    const double soc_ceiling = battery->has_max_soc ? battery->max_soc_kwh : battery->capacity_kwh;
    out->n = n;

    for(int i = 0; i < n; i++) {
        double load = load_kwh[i];
        double solar = solar_kwh[i];

        // stap 1: zon dekt eigen verbruik eerst
        double self_consumed_solar = dmin(solar, load);
        double remaining_load = load - self_consumed_solar;
        double solar_surplus = solar - self_consumed_solar;

        // stap 2: batterij dekt resterend verbruik (zelfconsumptie)
        double available = dmax(0.0, soc - battery->min_reserve_kwh);
        double discharge =
            dmin(dmin(discharge_plan[i], battery->max_discharge_kwh_per_interval), dmin(available, remaining_load));
        discharge = dmax(0.0, discharge);
        remaining_load -= discharge;

        // stap 3: laden clampen
        double desired_charge = dmax(charge_plan[i], solar_surplus);
        double room_left = dmax(0.0, soc_ceiling - (soc - discharge));
        if(room_left < SCHED_MIN_MEANINGFUL_CHARGE_KWH) {
            room_left = 0.0;
        }
        double room_in = battery->round_trip_efficiency > 0 ? room_left / battery->round_trip_efficiency : room_left;
        double charge = dmin(dmin(desired_charge, battery->max_charge_kwh_per_interval), room_in);
        charge = dmax(0.0, charge);

        // stap 4: laden eerst uit zon-overschot, dan van het net
        double solar_funded_charge = dmin(charge, solar_surplus);
        double grid_funded_charge = charge - solar_funded_charge;
        double exported_solar = solar_surplus - solar_funded_charge;

        soc += charge * battery->round_trip_efficiency;
        soc -= discharge;
        soc = dmin(dmax(soc, 0.0), soc_ceiling);

        double import_kwh = remaining_load + grid_funded_charge;
        double cost = import_kwh * import_prices[i];
        double revenue = exported_solar * export_prices[i];
        double degradation = (charge + discharge) * battery->degradation_cost_per_kwh;

        total_cost += cost - revenue + degradation;
        total_degradation += degradation;
        total_export_revenue += revenue;

        out->charge_plan[i] = charge;
        out->discharge_plan[i] = discharge;
        out->grid_import_kwh[i] = import_kwh;
        out->exported_kwh[i] = exported_solar;
        out->soc_trajectory[i] = soc;
    }
    out->total_cost_eur = round4(total_cost);
    out->total_degradation_eur = round4(total_degradation);
    out->total_export_revenue_eur = round4(total_export_revenue);
}

// ---------------------------------------------------------------------------
// Zoekfase in float.
//
// De ESP32-S3 heeft alleen een hardware-FPU voor float; double gaat in
// software (~50x trager). find_best_plan() draait ~12.000 simulaties van 192
// kwartieren - in double duurde dat >5 minuten. De zoektocht vergelijkt
// kandidaten met drempels van 0,001 EUR / 0,01 kWh; float-afrondfouten over
// 192 kwartieren blijven rond 1e-5, ruim daaronder. Het uiteindelijk gekozen
// plan wordt daarna in double doorgerekend (sched_simulate_plan), dus alle
// getoonde/teruggegeven getallen hebben gewoon double-precisie.
// ---------------------------------------------------------------------------

typedef struct {
    float capacity, max_charge, max_discharge, min_reserve, degradation, rte, soc_ceiling;
} batf_t;

typedef struct {
    float charge[SCHED_MAX_N];
    float discharge[SCHED_MAX_N];
    float total_cost; // afgerond op 4 decimalen, zoals de double-versie
} simf_t;

static inline float fminf2(float a, float b)
{
    return a < b ? a : b;
}

static inline float fmaxf2(float a, float b)
{
    return a > b ? a : b;
}

static void simulate_f(int n, const float * ip, const float * ep, const float * load, const float * solar,
                       const float * charge_plan, const float * discharge_plan, float soc0, const batf_t * b,
                       simf_t * out)
{
    const float min_charge = (float) SCHED_MIN_MEANINGFUL_CHARGE_KWH;
    float soc = soc0;
    float total = 0.0f;
    for(int i = 0; i < n; i++) {
        float self_consumed = fminf2(solar[i], load[i]);
        float remaining_load = load[i] - self_consumed;
        float surplus = solar[i] - self_consumed;

        float available = fmaxf2(0.0f, soc - b->min_reserve);
        float discharge = fminf2(fminf2(discharge_plan[i], b->max_discharge), fminf2(available, remaining_load));
        discharge = fmaxf2(0.0f, discharge);
        remaining_load -= discharge;

        float desired = fmaxf2(charge_plan[i], surplus);
        float room_left = fmaxf2(0.0f, b->soc_ceiling - (soc - discharge));
        if(room_left < min_charge) room_left = 0.0f;
        float room_in = b->rte > 0 ? room_left / b->rte : room_left;
        float charge = fmaxf2(0.0f, fminf2(fminf2(desired, b->max_charge), room_in));

        float solar_funded = fminf2(charge, surplus);
        float grid_funded = charge - solar_funded;
        float exported = surplus - solar_funded;

        soc += charge * b->rte;
        soc -= discharge;
        soc = fminf2(fmaxf2(soc, 0.0f), b->soc_ceiling);

        total += (remaining_load + grid_funded) * ip[i] - exported * ep[i] + (charge + discharge) * b->degradation;
        out->charge[i] = charge;
        out->discharge[i] = discharge;
    }
    out->total_cost = (float) round4((double) total);
}

// Kladruimte (PSRAM) - find_best_plan draait in één taak tegelijk.
typedef struct {
    float ip[SCHED_MAX_N], ep[SCHED_MAX_N], load[SCHED_MAX_N], solar[SCHED_MAX_N];
    simf_t best, trial;
    float charge_plan[SCHED_MAX_N];
    float discharge_plan[SCHED_MAX_N];
    float locked_charge_plan[SCHED_MAX_N];
    bool locked_block_mask[SCHED_MAX_N];
    bool mask[SCHED_MAX_N];
    double final_charge_plan[SCHED_MAX_N];
    double final_discharge_plan[SCHED_MAX_N];
    sched_sim_result_t unblocked;
} sched_scratch_t;

static sched_scratch_t * scratch(void)
{
    static sched_scratch_t * s = nullptr;
    if(!s) {
        s = (sched_scratch_t *) heap_caps_malloc(sizeof(sched_scratch_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return s;
}

static void discharge_plan_for(int n, const bool * mask, float max_discharge, float * out)
{
    for(int i = 0; i < n; i++) out[i] = mask[i] ? 0.0f : max_discharge;
}

// _best_single_window(): het ene extra laadvenster met de laagste kosten.
static void best_single_window(sched_scratch_t * s, int n, float soc0, const batf_t * b, const int * lengths,
                               int n_lengths, int * best_start, int * best_length, int * tried)
{
    for(int i = 0; i < n; i++) s->discharge_plan[i] = b->max_discharge;
    simulate_f(n, s->ip, s->ep, s->load, s->solar, s->locked_charge_plan, s->discharge_plan, soc0, b, &s->best);
    *best_start = -1;
    *best_length = -1;
    *tried = 1;
    const float min_charge = (float) SCHED_MIN_MEANINGFUL_CHARGE_KWH;
    const float min_improvement = (float) SCHED_MIN_MEANINGFUL_COST_IMPROVEMENT_EUR;

    for(int li = 0; li < n_lengths; li++) {
        int length = lengths[li];
        for(int start = 0; start <= n - length; start++) {
            bool occupied = false;
            for(int i = start; i < start + length; i++) {
                if(s->locked_charge_plan[i] > 0) {
                    occupied = true;
                    break;
                }
            }
            if(occupied) continue;

            memcpy(s->charge_plan, s->locked_charge_plan, n * sizeof(float));
            for(int i = start; i < start + length; i++) s->charge_plan[i] = b->max_charge;
            simulate_f(n, s->ip, s->ep, s->load, s->solar, s->charge_plan, s->discharge_plan, soc0, b, &s->trial);
            (*tried)++;
            if(s_yield && (*tried & 63) == 0) s_yield();

            float charge_added = 0.0f;
            for(int i = start; i < start + length; i++) charge_added += s->trial.charge[i] - s->best.charge[i];
            if(charge_added < min_charge) continue;
            if(s->best.total_cost - s->trial.total_cost < min_improvement) continue;
            if(s->trial.total_cost < s->best.total_cost) {
                s->best = s->trial;
                *best_start = start;
                *best_length = length;
            }
        }
    }
}

// _best_single_block_window(): het ene extra blokvenster met de laagste kosten.
static void best_single_block_window(sched_scratch_t * s, int n, float soc0, const batf_t * b, const int * lengths,
                                     int n_lengths, int * best_start, int * best_length, int * tried)
{
    discharge_plan_for(n, s->locked_block_mask, b->max_discharge, s->discharge_plan);
    simulate_f(n, s->ip, s->ep, s->load, s->solar, s->locked_charge_plan, s->discharge_plan, soc0, b, &s->best);
    *best_start = -1;
    *best_length = -1;
    *tried = 1;
    const float min_charge = (float) SCHED_MIN_MEANINGFUL_CHARGE_KWH;
    const float min_improvement = (float) SCHED_MIN_MEANINGFUL_COST_IMPROVEMENT_EUR;

    for(int li = 0; li < n_lengths; li++) {
        int length = lengths[li];
        for(int start = 0; start <= n - length; start++) {
            bool occupied = false;
            for(int i = start; i < start + length; i++) {
                if(s->locked_block_mask[i] || s->locked_charge_plan[i] > 0) {
                    occupied = true;
                    break;
                }
            }
            if(occupied) continue;

            memcpy(s->mask, s->locked_block_mask, n * sizeof(bool));
            for(int i = start; i < start + length; i++) s->mask[i] = true;
            discharge_plan_for(n, s->mask, b->max_discharge, s->discharge_plan);
            simulate_f(n, s->ip, s->ep, s->load, s->solar, s->locked_charge_plan, s->discharge_plan, soc0, b,
                       &s->trial);
            (*tried)++;
            if(s_yield && (*tried & 63) == 0) s_yield();

            float blocked = 0.0f;
            for(int i = start; i < start + length; i++) blocked += s->best.discharge[i] - s->trial.discharge[i];
            if(blocked < min_charge) continue;
            if(s->best.total_cost - s->trial.total_cost < min_improvement) continue;
            if(s->trial.total_cost < s->best.total_cost) {
                s->best = s->trial;
                *best_start = start;
                *best_length = length;
            }
        }
    }
}

void sched_find_best_plan(int n, const double * ip, const double * ep, const double * load, const double * solar,
                          double soc0, const sched_battery_t * b, const int * candidate_lengths, int n_lengths,
                          int max_windows, int max_block_windows, sched_sim_result_t * result_out,
                          sched_debug_t * dbg)
{
    static const int default_lengths[] = {1, 2, 3, 4, 5, 6, 7, 8};
    if(!candidate_lengths) {
        candidate_lengths = default_lengths;
        n_lengths = 8;
    }
    sched_scratch_t * s = scratch();
    memset(dbg, 0, sizeof(*dbg));

    batf_t bf;
    bf.capacity = (float) b->capacity_kwh;
    bf.max_charge = (float) b->max_charge_kwh_per_interval;
    bf.max_discharge = (float) b->max_discharge_kwh_per_interval;
    bf.min_reserve = (float) b->min_reserve_kwh;
    bf.degradation = (float) b->degradation_cost_per_kwh;
    bf.rte = (float) b->round_trip_efficiency;
    bf.soc_ceiling = (float) (b->has_max_soc ? b->max_soc_kwh : b->capacity_kwh);
    float soc0f = (float) soc0;

    for(int i = 0; i < n; i++) {
        s->ip[i] = (float) ip[i];
        s->ep[i] = (float) ep[i];
        s->load[i] = (float) load[i];
        s->solar[i] = (float) solar[i];
        s->locked_charge_plan[i] = 0.0f;
        s->locked_block_mask[i] = false;
    }

    for(int w = 0; w < max_windows; w++) {
        int start, length, tried;
        best_single_window(s, n, soc0f, &bf, candidate_lengths, n_lengths, &start, &length, &tried);
        dbg->scenarios_tried += tried;
        if(start < 0) break;
        for(int i = start; i < start + length; i++) s->locked_charge_plan[i] = bf.max_charge;
        if(dbg->n_windows < SCHED_MAX_WINDOWS) {
            dbg->windows_chosen[dbg->n_windows++] = {start, length};
        }
    }

    for(int w = 0; w < max_block_windows; w++) {
        int start, length, tried;
        best_single_block_window(s, n, soc0f, &bf, candidate_lengths, n_lengths, &start, &length, &tried);
        dbg->scenarios_tried += tried;
        if(start < 0) break;
        for(int i = start; i < start + length; i++) s->locked_block_mask[i] = true;
        dbg->block_windows_chosen[dbg->n_block_windows++] = {start, length};
    }

    // VEILIGHEIDSNET: nooit zelf ontladen op het goedkoopste moment
    // (onvoorwaardelijk, zie scheduler_core.py). Op de double-prijzen.
    const double CHEAPEST_PRICE_EPSILON_EUR = 1e-9;
    double min_price = ip[0];
    for(int i = 1; i < n; i++) min_price = dmin(min_price, ip[i]);
    if(max_block_windows > 0) {
        for(int i = 0; i < n; i++) {
            if(ip[i] > min_price + CHEAPEST_PRICE_EPSILON_EUR) continue;
            if(s->locked_charge_plan[i] > 0 || s->locked_block_mask[i]) continue;
            s->locked_block_mask[i] = true;
            dbg->block_windows_chosen[dbg->n_block_windows++] = {i, 1};
        }
    }

    // Eindresultaat in double: precies wat Python's current_result op dit
    // punt bevat - de simulatie van het vastgelegde laadplan + blokmasker.
    for(int i = 0; i < n; i++) {
        s->final_charge_plan[i] = s->locked_charge_plan[i] > 0 ? b->max_charge_kwh_per_interval : 0.0;
        s->final_discharge_plan[i] = s->locked_block_mask[i] ? 0.0 : b->max_discharge_kwh_per_interval;
    }
    sched_simulate_plan(n, ip, ep, load, solar, s->final_charge_plan, s->final_discharge_plan, soc0, b, result_out);

    // Puur weergave: voorkomen ontlading per geblokkeerd kwartier.
    bool any_block = false;
    for(int i = 0; i < n; i++) any_block = any_block || s->locked_block_mask[i];
    if(any_block) {
        for(int i = 0; i < n; i++) s->final_discharge_plan[i] = b->max_discharge_kwh_per_interval;
        sched_simulate_plan(n, ip, ep, load, solar, s->final_charge_plan, s->final_discharge_plan, soc0, b,
                            &s->unblocked);
        for(int i = 0; i < n; i++) {
            dbg->avoided_discharge_kwh[i] =
                s->locked_block_mask[i] ? dmax(0.0, s->unblocked.discharge_plan[i] - result_out->discharge_plan[i]) : 0.0;
        }
    }
}

void sched_interval_to_time(int interval_index, char * out, int out_size)
{
    int total = interval_index * SCHED_INTERVAL_MINUTES;
    snprintf(out, out_size, "%02d:%02d", (total / 60) % 24, total % 60);
}
