// Port van dashboard/test_scheduler_core.py, test_battery_scheduler.py en
// test_savings_tracker.py (EMS ae7029d) naar Unity, draait op het board:
//     pio test -e ems_test
// Zelfde invoer, zelfde verwachte uitkomsten als de Python-tests.
#include <Arduino.h>
#include <unity.h>

#include <esp_heap_caps.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "core/timeutil.h"
#include "logic/battery_scheduler.h"
#include "logic/savings_tracker.h"
#include "logic/scheduler_core.h"

static const double TEST_EPS = 1e-7; // assertAlmostEqual(places=7)

#define ASSERT_ALMOST(expected, actual) TEST_ASSERT_DOUBLE_WITHIN(TEST_EPS, (expected), (actual))

// ------------------------------------------------------------------ helpers

static double * filled(int n, double v)
{
    double * a = (double *) malloc(sizeof(double) * n);
    for(int i = 0; i < n; i++) a[i] = v;
    return a;
}

static double max_of(const double * a, int n)
{
    double m = a[0];
    for(int i = 1; i < n; i++) m = a[i] > m ? a[i] : m;
    return m;
}

static double min_of(const double * a, int n)
{
    double m = a[0];
    for(int i = 1; i < n; i++) m = a[i] < m ? a[i] : m;
    return m;
}

static sched_battery_t battery_with(double capacity, double max_charge, double max_discharge, double degradation,
                                    double rte)
{
    sched_battery_t b = sched_battery_default();
    b.capacity_kwh = capacity;
    b.max_charge_kwh_per_interval = max_charge;
    b.max_discharge_kwh_per_interval = max_discharge;
    b.degradation_cost_per_kwh = degradation;
    b.round_trip_efficiency = rte;
    return b;
}

static sched_sim_result_t g_sim, g_sim2;
static sched_debug_t g_dbg;

static void find_best(int n, const double * ip, const double * ep, const double * load, const double * solar,
                      double soc0, const sched_battery_t * b, int max_len, int max_block_windows,
                      sched_sim_result_t * out)
{
    int lengths[32];
    int n_lengths = 0;
    const int * lp = nullptr;
    if(max_len > 0) {
        for(int l = 1; l <= max_len; l++) lengths[n_lengths++] = l;
        lp = lengths;
    }
    sched_find_best_plan(n, ip, ep, load, solar, soc0, b, lp, n_lengths, 4, max_block_windows, out, &g_dbg);
}

static int idx(int hh, int mm)
{
    return (hh * 60 + mm) / 15;
}

// Python's datetime.datetime(2026, 9, 30, hh, mm) (naïef, lokale tijd).
static time_t at(int hh, int mm)
{
    return local_time_of({2026, 9, 30}, hh, mm);
}

// ===================================================== test_scheduler_core.py

static void test_never_exceeds_capacity(void)
{
    sched_battery_t b = sched_battery_default();
    b.capacity_kwh = 5.0;
    b.max_charge_kwh_per_interval = 1.0;
    int n = 10;
    double * prices = filled(n, 0.20);
    double * zero = filled(n, 0.0);
    double * charge = filled(n, 1.0);
    sched_simulate_plan(n, prices, prices, zero, zero, charge, zero, 4.5, &b, &g_sim);
    TEST_ASSERT_TRUE(max_of(g_sim.soc_trajectory, n) <= b.capacity_kwh + 1e-9);
    free(prices); free(zero); free(charge);
}

static void test_never_goes_negative(void)
{
    sched_battery_t b = sched_battery_default();
    b.capacity_kwh = 5.0;
    b.max_discharge_kwh_per_interval = 2.0;
    int n = 5;
    double * prices = filled(n, 0.20);
    double * zero = filled(n, 0.0);
    double * load = filled(n, 3.0);
    double * discharge = filled(n, 2.0);
    sched_simulate_plan(n, prices, prices, load, zero, zero, discharge, 1.0, &b, &g_sim);
    TEST_ASSERT_TRUE(min_of(g_sim.soc_trajectory, n) >= -1e-9);
    free(prices); free(zero); free(load); free(discharge);
}

static void test_doing_nothing_costs_exactly_price_times_load(void)
{
    sched_battery_t b = sched_battery_default();
    double ip[] = {0.10, 0.20, 0.30, 0.40};
    double ep[] = {0, 0, 0, 0};
    double load[] = {1, 1, 1, 1};
    double zero[] = {0, 0, 0, 0};
    sched_simulate_plan(4, ip, ep, load, zero, zero, zero, 0.0, &b, &g_sim);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.10 + 0.20 + 0.30 + 0.40, g_sim.total_cost_eur);
}

static void test_discharge_never_exceeds_actual_load(void)
{
    int n = 10;
    double * prices = filled(n, 0.30);
    prices[0] = 0.05;
    double * ep = filled(n, 0.0);
    double * load = filled(n, 0.0);
    load[5] = 1.0;
    double * solar = filled(n, 0.0);
    sched_battery_t b = battery_with(2.0, 1.0, 1.0, 0.0, 1.0);
    double * charge = filled(n, 0.0);
    charge[0] = 1.0;
    double * discharge = filled(n, 1.0);
    double * zero = filled(n, 0.0);

    sched_simulate_plan(n, prices, ep, load, solar, charge, discharge, 0.0, &b, &g_sim);
    sched_simulate_plan(n, prices, ep, load, solar, zero, zero, 0.0, &b, &g_sim2);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 1.0, g_sim.soc_trajectory[1]);
    TEST_ASSERT_TRUE(g_sim.total_cost_eur < g_sim2.total_cost_eur);
    free(prices); free(ep); free(load); free(solar); free(charge); free(discharge); free(zero);
}

static void test_two_peaks_charges_in_both_cheap_windows(void)
{
    int n = 96;
    double * prices = filled(n, 0.10);
    double * ep = filled(n, 0.0);
    double * load = filled(n, 0.0);
    double * solar = filled(n, 0.0);
    for(int i = idx(12, 0); i < idx(12, 30); i++) prices[i] = 0.35;
    load[idx(12, 0)] = 2.0;
    for(int i = idx(12, 30); i < idx(13, 0); i++) prices[i] = 0.20;
    for(int i = idx(13, 0); i < idx(13, 30); i++) prices[i] = 0.35;
    load[idx(13, 0)] = 2.0;
    for(int i = idx(13, 30); i < n; i++) prices[i] = 0.30;
    for(int i = idx(18, 0); i < idx(18, 0) + 12; i++) load[i] = 3.0 / 12;

    sched_battery_t b = battery_with(5.5, 0.625, 5.5, 0.0, 1.0);
    find_best(n, prices, ep, load, solar, 0.0, &b, 23, 4, &g_sim);

    double before_noon = 0, in_dip = 0;
    for(int i = 0; i < idx(12, 0); i++) before_noon += g_sim.charge_plan[i];
    for(int i = idx(12, 30); i < idx(13, 0); i++) in_dip += g_sim.charge_plan[i];
    TEST_ASSERT_TRUE_MESSAGE(before_noon > 0, "verwachtte laden in het goedkope ochtendvenster");
    TEST_ASSERT_TRUE_MESSAGE(in_dip > 0, "verwachtte OOK bijladen in het 12:30-13:00-tussendal");
    free(prices); free(ep); free(load); free(solar);
}

static void test_blocks_cheap_self_consumption_to_save_for_a_pricier_window(void)
{
    int n = 16;
    double ip[16];
    for(int i = 0; i < 16; i++) ip[i] = i < 8 ? 0.10 : 0.40;
    double * ep = filled(n, 0.0);
    double * load = filled(n, 1.0);
    double * solar = filled(n, 0.0);
    sched_battery_t b = battery_with(2.0, 2.0, 2.0, 0.05, 0.8);

    find_best(n, ip, ep, load, solar, 2.0, &b, 8, 4, &g_sim);
    int n_block = g_dbg.n_block_windows;
    find_best(n, ip, ep, load, solar, 2.0, &b, 8, 0, &g_sim2);

    TEST_ASSERT_TRUE_MESSAGE(n_block > 0, "verwachtte minstens één blokvenster");
    TEST_ASSERT_TRUE(g_sim.total_cost_eur < g_sim2.total_cost_eur);
    free(ep); free(load); free(solar);
}

static void test_does_not_charge_a_full_battery(void)
{
    int n = 96;
    sched_battery_t b = sched_battery_default();
    b.capacity_kwh = 5.12;
    b.max_charge_kwh_per_interval = 0.625;
    double ip[96];
    for(int i = 0; i < n; i++) ip[i] = 0.20 + 0.0000001 * (i % 7);
    double * ep = filled(n, 0.05);
    double * load = filled(n, 0.1);
    double * solar = filled(n, 0.0);

    find_best(n, ip, ep, load, solar, b.capacity_kwh, &b, 0, 4, &g_sim);
    TEST_ASSERT_EQUAL_INT(0, g_dbg.n_windows);
    double total = 0;
    for(int i = 0; i < n; i++) total += g_sim.charge_plan[i];
    TEST_ASSERT_TRUE(total < SCHED_MIN_MEANINGFUL_CHARGE_KWH);
    free(ep); free(load); free(solar);
}

static void test_solar_surplus_charges_battery_for_free_when_charging_requested(void)
{
    double ip[] = {0.30, 0.30, 0.30, 0.30};
    double ep[] = {0.10, 0.10, 0.10, 0.10};
    double load[] = {0, 0, 0, 0};
    double solar[] = {1.0, 1.0, 0.0, 0.0};
    sched_battery_t b = sched_battery_default();
    b.capacity_kwh = 5.0;
    b.max_charge_kwh_per_interval = 0.5;
    b.degradation_cost_per_kwh = 0.0;
    b.round_trip_efficiency = 1.0;
    double charge[] = {0.5, 0, 0, 0};
    double discharge[] = {0, 0, 0, 0};
    sched_simulate_plan(4, ip, ep, load, solar, charge, discharge, 0.0, &b, &g_sim);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, g_sim.grid_import_kwh[0]);
    TEST_ASSERT_TRUE(g_sim.exported_kwh[0] == 0.0 || g_sim.exported_kwh[0] == 0.5);
}

static void test_unused_solar_surplus_charges_battery_first_then_exports_the_rest(void)
{
    double ip[] = {0.30, 0.30};
    double ep[] = {0.08, 0.08};
    double load[] = {0, 0};
    double solar[] = {1.0, 0.0};
    sched_battery_t b = sched_battery_default();
    b.capacity_kwh = 5.0;
    b.max_charge_kwh_per_interval = 0.625;
    b.degradation_cost_per_kwh = 0.0;
    b.round_trip_efficiency = 1.0;
    double zero[] = {0, 0};
    sched_simulate_plan(2, ip, ep, load, solar, zero, zero, 0.0, &b, &g_sim);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.625, g_sim.charge_plan[0]);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.375, g_sim.exported_kwh[0]);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.375 * 0.08, g_sim.total_export_revenue_eur);
}

static void test_charging_from_solar_beats_charging_from_grid(void)
{
    double ip[] = {0.30, 0.30};
    double ep[] = {0.10, 0.10};
    double load[] = {0, 0};
    sched_battery_t b = sched_battery_default();
    b.capacity_kwh = 5.0;
    b.max_charge_kwh_per_interval = 1.0;
    b.degradation_cost_per_kwh = 0.0;
    b.round_trip_efficiency = 1.0;
    double charge[] = {1.0, 0.0};
    double discharge[] = {0, 0};
    double with_solar[] = {1.0, 0.0};
    double no_solar[] = {0.0, 0.0};
    sched_simulate_plan(2, ip, ep, load, with_solar, charge, discharge, 0.0, &b, &g_sim);
    sched_simulate_plan(2, ip, ep, load, no_solar, charge, discharge, 0.0, &b, &g_sim2);
    TEST_ASSERT_TRUE(g_sim.total_cost_eur < g_sim2.total_cost_eur);
}

static void cheapest_case(bool solar_covers_cheapest)
{
    int n = 10;
    double ip[] = {0.30, 0.30, 0.30, 0.05, 0.30, 0.30, 0.30, 0.40, 0.40, 0.40};
    double * ep = filled(n, 0.0);
    double * load = filled(n, 0.1);
    double * solar = filled(n, 0.0);
    if(solar_covers_cheapest) solar[3] = 0.1;
    sched_battery_t b = battery_with(5.0, 1.0, 1.0, 0.0, 1.0);
    find_best(n, ip, ep, load, solar, b.capacity_kwh, &b, 4, 4, &g_sim);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, g_sim.discharge_plan[3]);
    if(solar_covers_cheapest) {
        bool covered = false;
        for(int w = 0; w < g_dbg.n_block_windows; w++) {
            const sched_window_t & win = g_dbg.block_windows_chosen[w];
            if(win.start_interval <= 3 && 3 < win.start_interval + win.length_intervals) covered = true;
        }
        TEST_ASSERT_TRUE_MESSAGE(covered, "verwachtte een block-venster op kwartier 3");
    }
    free(ep); free(load); free(solar);
}

static void test_never_discharges_the_single_cheapest_interval_even_without_measurable_benefit(void)
{
    cheapest_case(false);
}

static void test_forces_a_block_at_the_cheapest_interval_even_when_the_forecast_shows_no_discharge_there(void)
{
    cheapest_case(true);
}

static void test_max_soc_kwh_caps_charging_below_capacity(void)
{
    sched_battery_t b = sched_battery_default();
    b.capacity_kwh = 5.0;
    b.max_charge_kwh_per_interval = 1.0;
    b.has_max_soc = true;
    b.max_soc_kwh = 4.0;
    int n = 10;
    double * prices = filled(n, 0.20);
    double * zero = filled(n, 0.0);
    double * charge = filled(n, 1.0);
    sched_simulate_plan(n, prices, prices, zero, zero, charge, zero, 0.0, &b, &g_sim);
    TEST_ASSERT_TRUE(max_of(g_sim.soc_trajectory, n) <= 4.0 + 1e-9);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 4.0, max_of(g_sim.soc_trajectory, n));
    free(prices); free(zero); free(charge);
}

static void test_max_soc_kwh_none_reproduces_old_full_capacity_ceiling(void)
{
    sched_battery_t b = sched_battery_default();
    b.capacity_kwh = 5.0;
    b.max_charge_kwh_per_interval = 1.0;
    int n = 10;
    double * prices = filled(n, 0.20);
    double * zero = filled(n, 0.0);
    double * charge = filled(n, 1.0);
    sched_simulate_plan(n, prices, prices, zero, zero, charge, zero, 0.0, &b, &g_sim);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 5.0, max_of(g_sim.soc_trajectory, n));
    free(prices); free(zero); free(charge);
}

static void test_find_best_plan_never_charges_above_max_soc_kwh(void)
{
    int n = 96;
    sched_battery_t b = sched_battery_default();
    b.capacity_kwh = 5.0;
    b.max_charge_kwh_per_interval = 0.625;
    b.has_max_soc = true;
    b.max_soc_kwh = 4.0;
    b.round_trip_efficiency = 1.0;
    double * ip = filled(n, 0.10);
    double * zero = filled(n, 0.0);
    find_best(n, ip, zero, zero, zero, 0.0, &b, 0, 4, &g_sim);
    TEST_ASSERT_TRUE(max_of(g_sim.soc_trajectory, n) <= 4.0 + 1e-9);
    free(ip); free(zero);
}

// ===================================================== test_battery_scheduler.py

static opt_d g_price[48], g_cons[192], g_solar[48];

static void fill_opt(opt_d * a, int n, double v)
{
    for(int i = 0; i < n; i++) a[i] = opt_some(v);
}

static plan_inputs_t inputs(int n_price, int n_cons, int n_solar)
{
    plan_inputs_t in = {};
    in.import_price_curve = n_price ? g_price : nullptr;
    in.n_price_hours = n_price;
    in.expected_consumption_curve = n_cons ? g_cons : nullptr;
    in.n_consumption = n_cons;
    in.solar_forecast_curve = n_solar ? g_solar : nullptr;
    in.n_solar_hours = n_solar;
    return in;
}

static plan_series_t g_series;
static plan_result_t * g_plan;

static void build(int n_price, int n_solar, time_t now, double horizon)
{
    plan_inputs_t in = inputs(n_price, 0, n_solar);
    plan_build_series(&in, now, horizon, &g_series);
}

static void test_import_price_is_epex_plus_inkoopvergoeding_plus_energiebelasting(void)
{
    fill_opt(g_price, 24, 0.10);
    build(24, 0, at(10, 0), 1.0);
    ASSERT_ALMOST(0.10 + INKOOPVERGOEDING_EUR_PER_KWH + ENERGIEBELASTING_EUR_PER_KWH, g_series.import_prices[0]);
}

static void test_export_excludes_salderen_unlike_import(void)
{
    fill_opt(g_price, 24, 0.10);
    fill_opt(g_solar, 24, 0.0);
    build(24, 24, at(10, 0), 1.0);
    ASSERT_ALMOST(0.10 + INKOOPVERGOEDING_EUR_PER_KWH, g_series.export_prices[0]);
    TEST_ASSERT_TRUE(fabs(g_series.export_prices[0] - g_series.import_prices[0]) > 1e-6);
}

static void test_zonnebonus_applied_when_solar_forecast_and_price_positive(void)
{
    fill_opt(g_price, 24, 0.10);
    fill_opt(g_solar, 24, 1.0);
    build(24, 24, at(12, 0), 1.0);
    double base = 0.10 + INKOOPVERGOEDING_EUR_PER_KWH;
    ASSERT_ALMOST(base + ZONNEBONUS_FRACTION * base, g_series.export_prices[0]);
}

static void test_zonnebonus_not_applied_when_bonus_base_negative(void)
{
    fill_opt(g_price, 24, -0.10);
    fill_opt(g_solar, 24, 1.0);
    build(24, 24, at(12, 0), 1.0);
    ASSERT_ALMOST(-0.10 + INKOOPVERGOEDING_EUR_PER_KWH, g_series.export_prices[0]);
}

static bool g_qvalid[QUARTERS_PER_PLAN];
static double g_qvalue[QUARTERS_PER_PLAN];

static void test_quarter_price_overrides_hourly_for_that_quarter_only(void)
{
    fill_opt(g_price, 24, 0.20);
    memset(g_qvalid, 0, sizeof(g_qvalid));
    g_qvalid[40] = true;
    g_qvalue[40] = 0.01;
    plan_inputs_t in = inputs(24, 0, 0);
    in.quarter_price_valid = g_qvalid;
    in.quarter_price_value = g_qvalue;
    in.n_quarter_prices = QUARTERS_PER_PLAN;
    plan_build_series(&in, at(10, 0), 1.0, &g_series);
    ASSERT_ALMOST(0.01 + INKOOPVERGOEDING_EUR_PER_KWH + ENERGIEBELASTING_EUR_PER_KWH, g_series.import_prices[0]);
    ASSERT_ALMOST(0.20 + INKOOPVERGOEDING_EUR_PER_KWH + ENERGIEBELASTING_EUR_PER_KWH, g_series.import_prices[1]);
}

static void test_quarter_price_can_extend_coverage_beyond_hourly_curve(void)
{
    fill_opt(g_price, 10, 0.0);
    g_price[10] = opt_some(0.20);
    memset(g_qvalid, 0, sizeof(g_qvalid));
    g_qvalid[44] = true;
    g_qvalue[44] = 0.05;
    plan_inputs_t in = inputs(11, 0, 0);
    in.quarter_price_valid = g_qvalid;
    in.quarter_price_value = g_qvalue;
    in.n_quarter_prices = QUARTERS_PER_PLAN;
    plan_build_series(&in, at(10, 0), 2.0, &g_series);
    int found = -1;
    for(int i = 0; i < g_series.n; i++) {
        if(g_series.quarters[i] == 44) found = i;
    }
    TEST_ASSERT_TRUE(found >= 0);
    ASSERT_ALMOST(0.05 + INKOOPVERGOEDING_EUR_PER_KWH + ENERGIEBELASTING_EUR_PER_KWH, g_series.import_prices[found]);
}

static void run_plan(int n_price, int n_cons, int n_solar, double soc, double capacity, double max_power, time_t now,
                     double min_soc = 12.0, double max_soc = 100.0, double loss = 5.0)
{
    plan_inputs_t in = inputs(n_price, n_cons, n_solar);
    plan_day(&in, soc, capacity, max_power, now, 48.0, min_soc, max_soc, loss, g_plan);
}

static bool any_positive(const opt_d * a, int from, int to)
{
    for(int q = from; q < to; q++) {
        if(a[q].valid && a[q].v > 0) return true;
    }
    return false;
}

static void test_plan_day_accepts_and_uses_quarter_price_curve(void)
{
    fill_opt(g_price, 24, 0.20);
    fill_opt(g_cons, 96, 0.3);
    fill_opt(g_solar, 24, 0.0);
    memset(g_qvalid, 0, sizeof(g_qvalid));
    g_qvalid[84] = true;
    g_qvalue[84] = 0.01;
    plan_inputs_t in = inputs(24, 96, 24);
    in.quarter_price_valid = g_qvalid;
    in.quarter_price_value = g_qvalue;
    in.n_quarter_prices = QUARTERS_PER_PLAN;
    plan_day(&in, 50.0, 5.0, 2000.0, at(20, 0), 48.0, 12.0, 100.0, 5.0, g_plan);
    TEST_ASSERT_TRUE(any_positive(g_plan->actief_gestuurd_wh, 84, 88));
}

static void test_no_price_curve_means_no_decision_and_empty_arrays(void)
{
    fill_opt(g_cons, 96, 0.1);
    fill_opt(g_solar, 24, 0.0);
    run_plan(0, 96, 24, 50.0, 5.0, 2000.0, at(20, 0));
    TEST_ASSERT_EQUAL_INT(DECISION_NONE, g_plan->current_decision.action);
    for(int q = 0; q < QUARTERS_PER_PLAN; q++) {
        TEST_ASSERT_FALSE(g_plan->eigen_zon_wh[q].valid);
        TEST_ASSERT_FALSE(g_plan->actief_gestuurd_wh[q].valid);
        TEST_ASSERT_FALSE(g_plan->soc_forecast_pct[q].valid);
    }
}

static void test_missing_consumption_and_solar_do_not_crash(void)
{
    fill_opt(g_price, 24, 0.20);
    run_plan(24, 0, 0, 50.0, 5.0, 2000.0, at(20, 0));
    TEST_ASSERT_NOT_EQUAL(DECISION_NONE, g_plan->current_decision.action);
}

// setUp van TestChargeOnlyDecision
static void charge_only_setup(void)
{
    fill_opt(g_price, 24, 0.20);
    g_price[21] = opt_some(0.05);
    fill_opt(g_cons, 96, 0.3);
    fill_opt(g_solar, 24, 0.0);
}

static void test_cheapest_hour_gets_a_charge_window(void)
{
    charge_only_setup();
    run_plan(24, 96, 24, 50.0, 5.0, 2000.0, at(20, 0));
    TEST_ASSERT_TRUE(any_positive(g_plan->actief_gestuurd_wh, 84, 88));
}

static void test_decision_is_auto_when_now_is_outside_the_chosen_window(void)
{
    charge_only_setup();
    run_plan(24, 96, 24, 50.0, 5.0, 2000.0, at(20, 0));
    TEST_ASSERT_EQUAL_INT(DECISION_AUTO, g_plan->current_decision.action);
}

static void test_decision_is_charge_when_now_is_inside_the_chosen_window(void)
{
    charge_only_setup();
    run_plan(24, 96, 24, 50.0, 5.0, 2000.0, at(21, 5));
    TEST_ASSERT_EQUAL_INT(DECISION_CHARGE, g_plan->current_decision.action);
    TEST_ASSERT_EQUAL_DOUBLE(-2000.0, g_plan->current_decision.power_w);
    TEST_ASSERT_TRUE(g_plan->current_decision.remaining_s > 0);
}

// --- nep-find_best_plan (Python: patch.object(scheduler_core, "find_best_plan")) ---
static sched_sim_result_t g_fake_result;
static sched_debug_t g_fake_debug;

static void fake_find_best_plan(int n, const double *, const double *, const double *, const double *, double,
                                const sched_battery_t *, const int *, int, int, int, sched_sim_result_t * result_out,
                                sched_debug_t * debug_out)
{
    (void) n;
    *result_out = g_fake_result;
    *debug_out = g_fake_debug;
}

static void fake_reset(double charge_q0)
{
    memset(&g_fake_result, 0, sizeof(g_fake_result));
    memset(&g_fake_debug, 0, sizeof(g_fake_debug));
    g_fake_result.n = SCHED_MAX_N;
    for(int i = 0; i < SCHED_MAX_N; i++) g_fake_result.soc_trajectory[i] = 2.5;
    g_fake_result.charge_plan[0] = charge_q0;
}

static void with_fake(time_t now)
{
    g_find_best_plan = fake_find_best_plan;
    run_plan(24, 96, 24, 50.0, 5.0, 2000.0, now);
    g_find_best_plan = sched_find_best_plan;
}

static void test_decision_is_charge_when_the_active_window_is_not_first_in_the_list(void)
{
    charge_only_setup();
    fake_reset(0.5);
    g_fake_debug.n_windows = 2;
    g_fake_debug.windows_chosen[0] = {12, 1};
    g_fake_debug.windows_chosen[1] = {0, 1};
    with_fake(at(20, 5));
    TEST_ASSERT_EQUAL_INT(DECISION_CHARGE, g_plan->current_decision.action);
}

static void test_decision_is_block_when_now_is_inside_a_chosen_block_window(void)
{
    charge_only_setup();
    fake_reset(0.0);
    g_fake_debug.n_block_windows = 2;
    g_fake_debug.block_windows_chosen[0] = {12, 1};
    g_fake_debug.block_windows_chosen[1] = {0, 3};
    with_fake(at(20, 5));
    TEST_ASSERT_EQUAL_INT(DECISION_BLOCK, g_plan->current_decision.action);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, g_plan->current_decision.power_w);
    TEST_ASSERT_TRUE(g_plan->current_decision.remaining_s > 0);
}

static void test_charge_window_takes_priority_over_an_overlapping_block_window(void)
{
    charge_only_setup();
    fake_reset(0.5);
    g_fake_debug.n_windows = 1;
    g_fake_debug.windows_chosen[0] = {0, 1};
    g_fake_debug.n_block_windows = 1;
    g_fake_debug.block_windows_chosen[0] = {0, 1};
    with_fake(at(20, 5));
    TEST_ASSERT_EQUAL_INT(DECISION_CHARGE, g_plan->current_decision.action);
}

static void test_geblokkeerd_wh_maps_avoided_discharge_kwh_per_quarter(void)
{
    charge_only_setup();
    fake_reset(0.0);
    g_fake_debug.n_block_windows = 1;
    g_fake_debug.block_windows_chosen[0] = {0, 2};
    g_fake_debug.avoided_discharge_kwh[0] = 0.3;
    g_fake_debug.avoided_discharge_kwh[1] = 0.3;
    with_fake(at(20, 0));
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 300.0, g_plan->geblokkeerd_wh[80].v);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 300.0, g_plan->geblokkeerd_wh[81].v);
    TEST_ASSERT_TRUE(g_plan->geblokkeerd_wh[82].valid);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, g_plan->geblokkeerd_wh[82].v);
}

static void test_geblokkeerd_wh_shows_verwacht_verbruik_even_without_avoided_discharge(void)
{
    charge_only_setup();
    fake_reset(0.0);
    g_fake_debug.n_block_windows = 1;
    g_fake_debug.block_windows_chosen[0] = {0, 1};
    with_fake(at(20, 0));
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 300.0, g_plan->geblokkeerd_wh[80].v);
}

static void test_start_time_marks_tomorrow_windows_to_disambiguate_from_today(void)
{
    charge_only_setup();
    fake_reset(0.0);
    g_fake_debug.n_windows = 1;
    g_fake_debug.windows_chosen[0] = {14, 1};
    g_fake_debug.n_block_windows = 1;
    g_fake_debug.block_windows_chosen[0] = {76, 1};
    with_fake(at(20, 0));
    TEST_ASSERT_EQUAL_STRING("23:30", g_plan->windows_start_time[0]);
    TEST_ASSERT_EQUAL_STRING("morgen 15:00", g_plan->block_windows_start_time[0]);
}

static void test_never_returns_a_discharge_action(void)
{
    charge_only_setup();
    for(int hour = 20; hour < 24; hour++) {
        run_plan(24, 96, 24, 50.0, 5.0, 2000.0, at(hour, 0));
        decision_action_t a = g_plan->current_decision.action;
        TEST_ASSERT_TRUE(a == DECISION_CHARGE || a == DECISION_BLOCK || a == DECISION_AUTO);
        if(a == DECISION_BLOCK) TEST_ASSERT_EQUAL_DOUBLE(0.0, g_plan->current_decision.power_w);
    }
}

static void test_no_solar_means_eigen_zon_is_zero_not_none_where_planned(void)
{
    charge_only_setup();
    run_plan(24, 96, 24, 50.0, 5.0, 2000.0, at(20, 0));
    for(int q = 80; q < 96; q++) {
        TEST_ASSERT_TRUE(g_plan->eigen_zon_wh[q].valid);
        TEST_ASSERT_EQUAL_DOUBLE(0.0, g_plan->eigen_zon_wh[q].v);
    }
}

static void test_hours_before_now_stay_none(void)
{
    charge_only_setup();
    run_plan(24, 96, 24, 50.0, 5.0, 2000.0, at(20, 0));
    for(int q = 0; q < 80; q++) {
        TEST_ASSERT_FALSE(g_plan->eigen_zon_wh[q].valid);
        TEST_ASSERT_FALSE(g_plan->soc_forecast_pct[q].valid);
    }
}

static void test_solar_surplus_shows_up_as_eigen_zon_outside_any_window(void)
{
    fill_opt(g_price, 24, 0.20);
    fill_opt(g_cons, 96, 0.05);
    fill_opt(g_solar, 24, 0.0);
    g_solar[13] = opt_some(3.0);
    run_plan(24, 96, 24, 50.0, 5.0, 2000.0, at(12, 0));
    TEST_ASSERT_TRUE(any_positive(g_plan->eigen_zon_wh, 52, 56));
}

static void test_truncates_to_today_only_when_no_tomorrow_data_given(void)
{
    fill_opt(g_price, 24, 0.20);
    fill_opt(g_cons, 96, 0.1);
    fill_opt(g_solar, 24, 0.0);
    run_plan(24, 96, 24, 50.0, 5.0, 2000.0, at(23, 30));
    TEST_ASSERT_NOT_EQUAL(DECISION_NONE, g_plan->current_decision.action);
    TEST_ASSERT_FALSE(g_plan->eigen_zon_wh[96].valid);
}

static void test_uses_tomorrow_hours_when_full_48h_data_is_given(void)
{
    fill_opt(g_price, 48, 0.20);
    g_price[30] = opt_some(0.05);
    fill_opt(g_cons, 192, 0.3);
    fill_opt(g_solar, 48, 0.0);
    run_plan(48, 192, 48, 50.0, 5.0, 2000.0, at(20, 0));
    bool any_valid = false;
    for(int q = 120; q < 124; q++) any_valid = any_valid || g_plan->actief_gestuurd_wh[q].valid;
    TEST_ASSERT_TRUE(any_valid);
    TEST_ASSERT_TRUE(any_positive(g_plan->actief_gestuurd_wh, 120, 124));
}

static void test_soc_forecast_spans_both_days(void)
{
    fill_opt(g_price, 48, 0.10);
    fill_opt(g_cons, 192, 0.05);
    fill_opt(g_solar, 48, 0.0);
    run_plan(48, 192, 48, 50.0, 5.0, 2000.0, at(20, 0));
    TEST_ASSERT_TRUE(g_plan->soc_forecast_pct[191].valid);
}

static sched_battery_t g_captured;

static void spy_find_best_plan(int n, const double * ip, const double * ep, const double * load,
                               const double * solar, double soc0, const sched_battery_t * b, const int * lengths,
                               int n_lengths, int max_windows, int max_block_windows, sched_sim_result_t * out,
                               sched_debug_t * dbg)
{
    g_captured = *b;
    sched_find_best_plan(n, ip, ep, load, solar, soc0, b, lengths, n_lengths, max_windows, max_block_windows, out, dbg);
}

static void capture(double min_soc, double max_soc, double loss)
{
    fill_opt(g_price, 24, 0.20);
    fill_opt(g_cons, 96, 0.3);
    fill_opt(g_solar, 24, 0.0);
    g_find_best_plan = spy_find_best_plan;
    run_plan(24, 96, 24, 50.0, 5.0, 2000.0, at(20, 0), min_soc, max_soc, loss);
    g_find_best_plan = sched_find_best_plan;
}

static void test_default_params_match_pre_2026_10_01_hardcoded_values(void)
{
    capture(12.0, 100.0, 5.0);
    ASSERT_ALMOST(5.0 * 0.12, g_captured.min_reserve_kwh);
    ASSERT_ALMOST(5.0 * 1.00, g_captured.max_soc_kwh);
    ASSERT_ALMOST(0.95, g_captured.round_trip_efficiency);
}

static void test_custom_params_are_threaded_through_to_batteryconfig(void)
{
    capture(20.0, 80.0, 10.0);
    ASSERT_ALMOST(5.0 * 0.20, g_captured.min_reserve_kwh);
    ASSERT_ALMOST(5.0 * 0.80, g_captured.max_soc_kwh);
    ASSERT_ALMOST(0.90, g_captured.round_trip_efficiency);
}

// ===================================================== test_savings_tracker.py

static void test_charge_fully_from_solar_surplus(void)
{
    charge_result_t r = savings_charge({0.0}, 1.0, 5.0, 2.0, 0.10, 0.05, true);
    ASSERT_ALMOST(1.0, r.state.shadow_soc_kwh);
    ASSERT_ALMOST(-0.10, r.batterij_cash_flow_eur);
    ASSERT_ALMOST(0.0, r.ems_cash_flow_eur);
}

static void test_charge_fully_actief_when_no_solar_inside_a_window(void)
{
    charge_result_t r = savings_charge({0.0}, 1.0, 5.0, 0.0, 0.10, 0.05, true);
    ASSERT_ALMOST(0.0, r.state.shadow_soc_kwh);
    ASSERT_ALMOST(0.0, r.batterij_cash_flow_eur);
    ASSERT_ALMOST(-0.05, r.ems_cash_flow_eur);
}

static void test_charge_split_when_solar_partially_covers_an_active_window(void)
{
    charge_result_t r = savings_charge({0.0}, 2.5, 5.0, 0.5, 0.10, 0.05, true);
    ASSERT_ALMOST(0.5, r.state.shadow_soc_kwh);
    ASSERT_ALMOST(-0.5 * 0.10, r.batterij_cash_flow_eur);
    ASSERT_ALMOST(-2.0 * 0.05, r.ems_cash_flow_eur);
}

static void test_charge_capped_by_shadow_headroom_not_just_solar_surplus(void)
{
    charge_result_t r = savings_charge({4.8}, 1.0, 5.0, 1.0, 0.10, 0.05, true);
    ASSERT_ALMOST(5.0, r.state.shadow_soc_kwh);
    ASSERT_ALMOST(-0.2 * 0.10, r.batterij_cash_flow_eur);
    ASSERT_ALMOST(-0.8 * 0.05, r.ems_cash_flow_eur);
}

static void test_charge_mid_cycle_before_discharge_is_already_negative(void)
{
    charge_result_t r = savings_charge({0.0}, 1.0, 5.0, 0.0, 0.10, 0.05, true);
    TEST_ASSERT_TRUE(r.ems_cash_flow_eur < 0.0);
}

static void test_charge_zero_or_negative_kwh_is_a_no_op(void)
{
    charge_result_t r = savings_charge({1.0}, 0.0, 5.0, 2.0, 0.10, 0.05, true);
    TEST_ASSERT_EQUAL_DOUBLE(1.0, r.state.shadow_soc_kwh);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, r.batterij_cash_flow_eur);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, r.ems_cash_flow_eur);
}

static void test_charge_outside_a_window_is_fully_zon_even_with_zero_solar_estimate(void)
{
    charge_result_t r = savings_charge({0.0}, 1.0, 5.0, 0.0, 0.10, 0.05, false);
    ASSERT_ALMOST(1.0, r.state.shadow_soc_kwh);
    ASSERT_ALMOST(-0.10, r.batterij_cash_flow_eur);
    ASSERT_ALMOST(0.0, r.ems_cash_flow_eur);
}

static void test_charge_outside_a_window_ignores_solar_surplus_value_entirely(void)
{
    charge_result_t r = savings_charge({0.0}, 2.0, 5.0, 0.3, 0.10, 0.05, false);
    ASSERT_ALMOST(2.0, r.state.shadow_soc_kwh);
    ASSERT_ALMOST(-2.0 * 0.10, r.batterij_cash_flow_eur);
    ASSERT_ALMOST(0.0, r.ems_cash_flow_eur);
}

static void test_charge_outside_a_window_still_capped_by_shadow_headroom(void)
{
    charge_result_t r = savings_charge({4.8}, 1.0, 5.0, 0.0, 0.10, 0.05, false);
    ASSERT_ALMOST(5.0, r.state.shadow_soc_kwh);
    ASSERT_ALMOST(-0.2 * 0.10, r.batterij_cash_flow_eur);
    ASSERT_ALMOST(-0.8 * 0.05, r.ems_cash_flow_eur);
}

static void test_discharge_fully_coverable_by_shadow(void)
{
    discharge_result_t r = savings_discharge({2.0}, 1.0, 0.30);
    ASSERT_ALMOST(1.0, r.state.shadow_soc_kwh);
    ASSERT_ALMOST(0.30, r.batterij_savings_eur);
    ASSERT_ALMOST(0.0, r.ems_savings_eur);
}

static void test_discharge_fully_beyond_shadow_is_all_ems(void)
{
    discharge_result_t r = savings_discharge({0.0}, 1.0, 0.30);
    ASSERT_ALMOST(0.0, r.state.shadow_soc_kwh);
    ASSERT_ALMOST(0.0, r.batterij_savings_eur);
    ASSERT_ALMOST(0.30, r.ems_savings_eur);
}

static void test_discharge_partially_coverable_splits_correctly(void)
{
    discharge_result_t r = savings_discharge({0.6}, 1.0, 0.30);
    ASSERT_ALMOST(0.0, r.state.shadow_soc_kwh);
    ASSERT_ALMOST(0.6 * 0.30, r.batterij_savings_eur);
    ASSERT_ALMOST(0.4 * 0.30, r.ems_savings_eur);
}

static void test_discharge_zero_or_negative_kwh_is_a_no_op(void)
{
    discharge_result_t r = savings_discharge({1.0}, 0.0, 0.30);
    TEST_ASSERT_EQUAL_DOUBLE(1.0, r.state.shadow_soc_kwh);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, r.batterij_savings_eur);
}

static void test_full_cycle_worked_example(void)
{
    charge_result_t c1 = savings_charge({0.0}, 1.0, 5.0, 1.0, 0.10, 0.05, false);
    charge_result_t c2 = savings_charge(c1.state, 1.0, 5.0, 0.0, 0.10, 0.05, true);
    ASSERT_ALMOST(1.0, c2.state.shadow_soc_kwh);
    discharge_result_t d = savings_discharge(c2.state, 2.0, 0.30);
    ASSERT_ALMOST(1.0 * 0.30, d.batterij_savings_eur);
    ASSERT_ALMOST(1.0 * 0.30, d.ems_savings_eur);
    ASSERT_ALMOST(0.0, d.state.shadow_soc_kwh);
    ASSERT_ALMOST(-0.10 + 0.30, c1.batterij_cash_flow_eur + d.batterij_savings_eur);
    ASSERT_ALMOST(-0.05 + 0.30, c2.ems_cash_flow_eur + d.ems_savings_eur);
}

static void test_block_fully_coverable_drains_shadow_no_cashflow(void)
{
    ASSERT_ALMOST(1.0, savings_block({2.0}, 1.0).shadow_soc_kwh);
}

static void test_block_beyond_shadow_floors_at_zero(void)
{
    ASSERT_ALMOST(0.0, savings_block({0.6}, 1.0).shadow_soc_kwh);
}

static void test_block_with_empty_shadow_is_a_no_op(void)
{
    TEST_ASSERT_EQUAL_DOUBLE(0.0, savings_block({0.0}, 1.0).shadow_soc_kwh);
}

static void test_block_zero_or_negative_kwh_is_a_no_op(void)
{
    TEST_ASSERT_EQUAL_DOUBLE(1.0, savings_block({1.0}, 0.0).shadow_soc_kwh);
    TEST_ASSERT_EQUAL_DOUBLE(1.0, savings_block({1.0}, -1.0).shadow_soc_kwh);
}

static void test_block_then_later_discharge_credits_ems_not_batterij(void)
{
    charge_result_t c = savings_charge({0.0}, 1.0, 5.0, 1.0, 0.10, 0.05, false);
    ASSERT_ALMOST(1.0, c.state.shadow_soc_kwh);
    shadow_state_t blocked = savings_block(c.state, 1.0);
    ASSERT_ALMOST(0.0, blocked.shadow_soc_kwh);
    discharge_result_t d = savings_discharge(blocked, 1.0, 0.35);
    ASSERT_ALMOST(0.0, d.batterij_savings_eur);
    ASSERT_ALMOST(0.35, d.ems_savings_eur);
}

// ------------------------------------------------------------------ runner

void setUp(void) {}
void tearDown(void) {}

void setup()
{
    delay(2000); // tijd voor de seriële monitor van `pio test`
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    g_plan = (plan_result_t *) heap_caps_malloc(sizeof(plan_result_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    UNITY_BEGIN();
    // test_scheduler_core.py
    RUN_TEST(test_never_exceeds_capacity);
    RUN_TEST(test_never_goes_negative);
    RUN_TEST(test_doing_nothing_costs_exactly_price_times_load);
    RUN_TEST(test_discharge_never_exceeds_actual_load);
    RUN_TEST(test_two_peaks_charges_in_both_cheap_windows);
    RUN_TEST(test_blocks_cheap_self_consumption_to_save_for_a_pricier_window);
    RUN_TEST(test_does_not_charge_a_full_battery);
    RUN_TEST(test_solar_surplus_charges_battery_for_free_when_charging_requested);
    RUN_TEST(test_unused_solar_surplus_charges_battery_first_then_exports_the_rest);
    RUN_TEST(test_charging_from_solar_beats_charging_from_grid);
    RUN_TEST(test_never_discharges_the_single_cheapest_interval_even_without_measurable_benefit);
    RUN_TEST(test_forces_a_block_at_the_cheapest_interval_even_when_the_forecast_shows_no_discharge_there);
    RUN_TEST(test_max_soc_kwh_caps_charging_below_capacity);
    RUN_TEST(test_max_soc_kwh_none_reproduces_old_full_capacity_ceiling);
    RUN_TEST(test_find_best_plan_never_charges_above_max_soc_kwh);
    // test_battery_scheduler.py
    RUN_TEST(test_import_price_is_epex_plus_inkoopvergoeding_plus_energiebelasting);
    RUN_TEST(test_export_excludes_salderen_unlike_import);
    RUN_TEST(test_zonnebonus_applied_when_solar_forecast_and_price_positive);
    RUN_TEST(test_zonnebonus_not_applied_when_bonus_base_negative);
    RUN_TEST(test_quarter_price_overrides_hourly_for_that_quarter_only);
    RUN_TEST(test_quarter_price_can_extend_coverage_beyond_hourly_curve);
    RUN_TEST(test_plan_day_accepts_and_uses_quarter_price_curve);
    RUN_TEST(test_no_price_curve_means_no_decision_and_empty_arrays);
    RUN_TEST(test_missing_consumption_and_solar_do_not_crash);
    RUN_TEST(test_cheapest_hour_gets_a_charge_window);
    RUN_TEST(test_decision_is_auto_when_now_is_outside_the_chosen_window);
    RUN_TEST(test_decision_is_charge_when_now_is_inside_the_chosen_window);
    RUN_TEST(test_decision_is_charge_when_the_active_window_is_not_first_in_the_list);
    RUN_TEST(test_decision_is_block_when_now_is_inside_a_chosen_block_window);
    RUN_TEST(test_charge_window_takes_priority_over_an_overlapping_block_window);
    RUN_TEST(test_geblokkeerd_wh_maps_avoided_discharge_kwh_per_quarter);
    RUN_TEST(test_geblokkeerd_wh_shows_verwacht_verbruik_even_without_avoided_discharge);
    RUN_TEST(test_start_time_marks_tomorrow_windows_to_disambiguate_from_today);
    RUN_TEST(test_never_returns_a_discharge_action);
    RUN_TEST(test_no_solar_means_eigen_zon_is_zero_not_none_where_planned);
    RUN_TEST(test_hours_before_now_stay_none);
    RUN_TEST(test_solar_surplus_shows_up_as_eigen_zon_outside_any_window);
    RUN_TEST(test_truncates_to_today_only_when_no_tomorrow_data_given);
    RUN_TEST(test_uses_tomorrow_hours_when_full_48h_data_is_given);
    RUN_TEST(test_soc_forecast_spans_both_days);
    RUN_TEST(test_default_params_match_pre_2026_10_01_hardcoded_values);
    RUN_TEST(test_custom_params_are_threaded_through_to_batteryconfig);
    // test_savings_tracker.py
    RUN_TEST(test_charge_fully_from_solar_surplus);
    RUN_TEST(test_charge_fully_actief_when_no_solar_inside_a_window);
    RUN_TEST(test_charge_split_when_solar_partially_covers_an_active_window);
    RUN_TEST(test_charge_capped_by_shadow_headroom_not_just_solar_surplus);
    RUN_TEST(test_charge_mid_cycle_before_discharge_is_already_negative);
    RUN_TEST(test_charge_zero_or_negative_kwh_is_a_no_op);
    RUN_TEST(test_charge_outside_a_window_is_fully_zon_even_with_zero_solar_estimate);
    RUN_TEST(test_charge_outside_a_window_ignores_solar_surplus_value_entirely);
    RUN_TEST(test_charge_outside_a_window_still_capped_by_shadow_headroom);
    RUN_TEST(test_discharge_fully_coverable_by_shadow);
    RUN_TEST(test_discharge_fully_beyond_shadow_is_all_ems);
    RUN_TEST(test_discharge_partially_coverable_splits_correctly);
    RUN_TEST(test_discharge_zero_or_negative_kwh_is_a_no_op);
    RUN_TEST(test_full_cycle_worked_example);
    RUN_TEST(test_block_fully_coverable_drains_shadow_no_cashflow);
    RUN_TEST(test_block_beyond_shadow_floors_at_zero);
    RUN_TEST(test_block_with_empty_shadow_is_a_no_op);
    RUN_TEST(test_block_zero_or_negative_kwh_is_a_no_op);
    RUN_TEST(test_block_then_later_discharge_credits_ems_not_batterij);
    UNITY_END();
}

void loop() {}
