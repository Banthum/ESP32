#include "screen_price.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "screen_common.h"
#include "fonts_signage.h"

/* 2-daags (vandaag+morgen), kwartier-resolutie (2026-10-08, Jeroen: "ik
 * wil dat de energiekostentabel voor twee dagen laat zien") - zelfde
 * 192-puntsconventie als screen_solar.c/screen_battery.c. */
#define N_QUARTERS IPC_BATTERY_PLAN_LEN

#define AXIS_MIN_MEUR 0    /* milli-EUR/kWh, i.e. price * 1000 (matches screen_main.c's convention) */
#define AXIS_MAX_MEUR 500  /* placeholder until the first real price_curve rescales this, see rescale_price_axis() below */
/* Rounding step (EUR/kWh) for the dynamic primary-axis rescale below -
 * 2026-10-01, Jeroen: the all-in price formula pushed real prices above
 * this fixed 0.5 ceiling (peak-hour prices now ~0.57), clipping the chart
 * - same "vaste aanname, niet afgeleid van echte klantdata" gap CLAUDE.md
 * already flagged for this screen, now actually triggered. Mirrors
 * screen_solar.c's axis_max_wh_for_kwp() rounding idea, just a finer step
 * (0.1 EUR/kWh, appropriate for this screen's 0-1ish EUR/kWh range vs.
 * solar's 0.5 kWp steps for a multi-kWp range). */
#define PRICE_AXIS_ROUND_STEP_EUR 0.1

/* Gas axis range depends on the configured unit (added 2026-09-27) - see
 * rescale_gas_axis() below. In EUR/kWh mode, gas shares electricity's own
 * 0-0.5 range exactly (same AXIS_MAX_MEUR) so both axes' gridlines line up
 * and the two are directly visually comparable, not just coincidentally
 * similar-looking. In EUR/m3 mode gas needs real headroom above typical
 * NL retail rates (~0.80-1.50 EUR/m3). */
#define GAS_AXIS_MAX_MEUR_KWH AXIS_MAX_MEUR
#define GAS_AXIS_MAX_MEUR_M3 2000

/* Named so rescale_gas_axis() can't drift out of sync with
 * screen_price_build()'s actual chart position/size the way a
 * copy-pasted literal did before (live-confirmed: the legend below
 * overlapped the Y-axis's top value label because this shifted from 34 to
 * 54 without updating rescale_gas_axis()'s own hardcoded copy). x/width
 * mirror screen_common_add_chart_tooltip()'s hit-testing needs, y/height
 * are shared by both the left and right axis-label calls. */
/* ESP32 (800x480): marges voor de as-labels schalen met de lettergrootte
 * (UI_S), de grafiek zelf vult de rest van de breedte (zelfde 56px links /
 * 60px rechts als op de Pi, maar dan 1,5x). */
#define CHART_X UI_S(56)
#define CHART_WIDTH (UI_SCREEN_W - CHART_X - UI_S(60))
#define CHART_Y UI_SY(54) /* was 34 - +20 to clear the legend row added below it */
#define CHART_HEIGHT UI_SY(160) /* was 180 - -20 to keep the same bottom edge (54+160 == 34+180) */
#define CHART_RIGHT_X (CHART_X + CHART_WIDTH)

static lv_obj_t * g_chart;
static lv_chart_series_t * g_series;
static lv_chart_series_t * g_export_series;
static lv_chart_series_t * g_gas_series;
static lv_chart_cursor_t * g_now_cursor;
static lv_chart_cursor_t * g_day_boundary_cursor; /* fixed at kwartier 96 (middernacht) */
static lv_obj_t * g_empty_label;
static lv_obj_t * g_legend;
static lv_obj_t * g_axis_labels[3]; /* [max, mid, min], see screen_common_add_y_axis_labels() */
static lv_obj_t * g_gas_axis_labels[3]; /* [max, mid, min], see screen_common_add_y_axis_labels_right() */
static lv_obj_t * g_gas_unit_label;

/* Last-applied primary-axis range, in EUR/kWh (not yet rounded to the
 * milli-EUR int32 the chart API wants) - NAN sentinel, same reasoning as
 * screen_solar.c's g_axis_applied_for_kwp, so the very first real price
 * (including a legitimate exact match to the AXIS_MAX_MEUR default) still
 * triggers the initial rescale/relabel. */
static double g_axis_applied_min_eur = NAN;
static double g_axis_applied_max_eur = NAN;

/* Cached from the last screen_price_update() call, for the tap-tooltip
 * (added 2026-09-27) - a chart doesn't otherwise retain the data behind
 * its rendered series between _update() calls, and a tap can happen at
 * any time, not just right after an update. Quarter-resolution, 2-daags
 * (N_QUARTERS=192, vandaag+morgen, sinds 2026-10-08 - was 96/today-only)
 * now, not hourly - see screen_price_build()'s chart point_count.
 * Per-element valid (not one blanket flag), mirrors
 * price_curve_quarterly_valid's own per-element shape - morgen's prijs
 * kan voor een deel van de curve nog ontbreken. gas stays hourly (24) in
 * the IPC message (no day-ahead quarter market for gas, see
 * price_client.py) and has no "morgen"-fetch at all - expanded here by
 * repeating its 24 values across both days (modulo 24) so it can share
 * the chart's single point_count with the other two series. */
static bool g_price_curve_valid[N_QUARTERS];
static double g_price_curve[N_QUARTERS];
static bool g_export_price_curve_valid[N_QUARTERS];
static double g_export_price_curve[N_QUARTERS];
static bool g_gas_visible; /* gas_enabled AND gas_price_curve_valid, see screen_price_update() */
static double g_gas_curve[N_QUARTERS];
static char g_gas_unit[IPC_STRING_MAX];

/* "" (never a valid gas_price_unit value) so the very first real unit -
 * "EUR/kWh" or "EUR/m3" - always differs and triggers the initial
 * rescale/relabel, same NAN-sentinel idea as screen_solar.c's
 * g_axis_applied_for_kwp (just string-flavored, since the thing that
 * changes here is a unit choice, not a numeric range on its own). */
static char g_gas_unit_applied[IPC_STRING_MAX] = "";

/* Tracks whether the legend text currently includes the "gas" entry, so
 * screen_price_update() only rewrites it on an actual gas_visible
 * transition rather than every 10s-poll (same "recompute only on change"
 * style as g_gas_unit_applied above). -1 (not 0/1) so the very first
 * update always applies, matching this file's NAN-sentinel-equivalent
 * convention. */
static int g_legend_gas_applied = -1;

static void price_tooltip_fill(void * user_data, int index, char * buf, size_t buf_size)
{
    (void) user_data;
    int quarter_of_day = index % IPC_QUARTERS_PER_DAY;
    int hour = (quarter_of_day * 15) / 60;
    int minute = (quarter_of_day * 15) % 60;
    const char * day_prefix = index >= IPC_QUARTERS_PER_DAY ? "Morgen " : "";

    char price_buf[24];
    if(g_price_curve_valid[index]) snprintf(price_buf, sizeof(price_buf), "%.2f EUR/kWh", g_price_curve[index]);
    else snprintf(price_buf, sizeof(price_buf), "geen data");

    char export_buf[24];
    if(g_export_price_curve_valid[index]) snprintf(export_buf, sizeof(export_buf), "%.2f EUR/kWh", g_export_price_curve[index]);
    else snprintf(export_buf, sizeof(export_buf), "geen data");

    if(!g_gas_visible) {
        snprintf(buf, buf_size, "%s%02d:%02d\nInkoop: %s\nTerug: %s", day_prefix, hour, minute, price_buf, export_buf);
        return;
    }

    char gas_buf[24];
    snprintf(gas_buf, sizeof(gas_buf), "%.2f %s", g_gas_curve[index], g_gas_unit);
    snprintf(buf, buf_size, "%s%02d:%02d\nInkoop: %s\nTerug: %s\nGas: %s", day_prefix, hour, minute, price_buf,
             export_buf, gas_buf);
}

/* Computes the primary (stroom) axis range that fits both price_curve and
 * export_price_curve with no clipping, rounded to PRICE_AXIS_ROUND_STEP_EUR
 * steps - mirrors screen_solar.c's axis_max_wh_for_kwp() rounding idea,
 * but driven by the actual observed data range (there's no config-value
 * equivalent to kWp/capacity_kwh to scale against here) rather than a
 * known ceiling. Min stays pinned at 0 unless a real price actually went
 * negative (EPEX can) - extending below 0 "for symmetry" when all prices
 * are positive would just waste vertical chart space. Returns false (no
 * valid data to scale to) if neither curve is valid - caller keeps
 * whatever range was last applied in that case. */
static bool compute_price_axis_range(const dashboard_state_t * state, double * out_min_eur, double * out_max_eur)
{
    double lo = 0.0, hi = 0.0;
    bool any = false;
    for(int i = 0; i < N_QUARTERS; i++) {
        if(state->price_curve_quarterly_valid[i]) {
            double v = state->price_curve_quarterly[i];
            if(!any || v < lo) lo = v;
            if(!any || v > hi) hi = v;
            any = true;
        }
        if(state->export_price_curve_quarterly_valid[i]) {
            double v = state->export_price_curve_quarterly[i];
            if(!any || v < lo) lo = v;
            if(!any || v > hi) hi = v;
            any = true;
        }
    }
    if(!any) return false;

    if(lo > 0.0) lo = 0.0; /* never show a positive-only range as starting above 0 */
    *out_min_eur = (lo < 0.0) ? floor(lo / PRICE_AXIS_ROUND_STEP_EUR) * PRICE_AXIS_ROUND_STEP_EUR : 0.0;
    double rounded_max = ceil(hi / PRICE_AXIS_ROUND_STEP_EUR) * PRICE_AXIS_ROUND_STEP_EUR;
    *out_max_eur = (rounded_max > *out_min_eur) ? rounded_max : *out_min_eur + PRICE_AXIS_ROUND_STEP_EUR;
    return true;
}

/* Rescales the gas (right/secondary) Y-axis to `unit`'s range and
 * relabels its unit tag - mirrors screen_solar.c's kWp-driven rescale
 * pattern, just keyed on a unit string instead of a numeric kWp. Called
 * once per distinct unit value rather than every 10s-poll, same
 * "customer edit is rare, don't recompute needlessly" reasoning. */
static void rescale_gas_axis(const char * unit)
{
    int32_t axis_max = (strcmp(unit, "EUR/m3") == 0) ? GAS_AXIS_MAX_MEUR_M3 : GAS_AXIS_MAX_MEUR_KWH;
    lv_chart_set_axis_range(g_chart, LV_CHART_AXIS_SECONDARY_Y, AXIS_MIN_MEUR, axis_max);
    screen_common_set_axis_label_value(g_gas_axis_labels[0], axis_max, 1000);
    screen_common_set_axis_label_value(g_gas_axis_labels[1], axis_max / 2, 1000);
    screen_common_set_axis_label_value(g_gas_axis_labels[2], AXIS_MIN_MEUR, 1000);
    screen_common_set_axis_unit_label(g_gas_unit_label, unit, CHART_RIGHT_X + UI_S(46), CHART_Y + CHART_HEIGHT / 2);
    snprintf(g_gas_unit_applied, sizeof(g_gas_unit_applied), "%s", unit);
}

void screen_price_build(lv_obj_t * parent)
{
    lv_obj_t * title = lv_label_create(parent);
    lv_label_set_text_static(title, "Energiekosten (EUR/kWh)");
    lv_obj_set_pos(title, UI_SX(10), UI_SY(4));
    lv_obj_set_style_text_font(title, &lv_font_signage_18, 0);

    /* Legend - always shown once price data exists (see
     * screen_price_update()): 2026-10-01 added a 2nd always-on series
     * (terugleverprijs), so unlike before there's no longer a
     * single-series case where a legend would be unnecessary clutter.
     * "gas" is appended dynamically only while actually visible, see
     * screen_price_update(). Hex values are R/B-swapped (BGR-wiring
     * quirk, same as bgr_fix() but recolor tags take raw hex text): blue
     * (0,0,255)->ff0000, green (0,255,0)->00ff00 (green=255 exactly, safe
     * under the fb_ili9486 scramble rule - see CLAUDE.md "Scherm"), orange
     * (255,152,0)->0098ff. */
    g_legend = lv_label_create(parent);
    lv_label_set_recolor(g_legend, true);
    lv_label_set_text_static(g_legend, "#0000ff inkoop# / #00ff00 teruglevering#");
    lv_obj_set_style_text_font(g_legend, &lv_font_signage_14, 0);
    lv_obj_set_pos(g_legend, UI_SX(10), UI_SY(26));
    lv_obj_set_hidden(g_legend, true);

    /* x=56/right-edge=420 (not the usual 10/460, nor the previous
     * 56/470) - leaves room on both sides now: left for
     * screen_common_add_y_axis_labels()'s rotated EUR/kWh unit tag +
     * number column, right for screen_common_add_y_axis_labels_right()'s
     * gas column (always reserved whether gas is currently shown or not,
     * same static-layout simplicity trade-off as screen_battery.c's axes).
     * y=54 (not the previous 34) - the legend row above needs the extra
     * 20px, see CHART_Y's comment. */
    g_chart = lv_chart_create(parent);
    lv_obj_set_pos(g_chart, CHART_X, CHART_Y);
    lv_obj_set_size(g_chart, CHART_WIDTH, CHART_HEIGHT);
    lv_chart_set_type(g_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(g_chart, N_QUARTERS);
    lv_chart_set_axis_range(g_chart, LV_CHART_AXIS_PRIMARY_Y, AXIS_MIN_MEUR, AXIS_MAX_MEUR);
    lv_chart_set_axis_range(g_chart, LV_CHART_AXIS_SECONDARY_Y, AXIS_MIN_MEUR, GAS_AXIS_MAX_MEUR_KWH);

    /* Pure blue (0,0,255), not LV_PALETTE_BLUE (33,150,243) - the latter
     * fails all three fb_ili9486 scramble-safety conditions (see
     * CLAUDE.md "Scherm" section / the cursor comment below) and rendered
     * wrong on real hardware. Same pure blue as the "now" cursor below -
     * distinguished from it by line style (solid+dots vs. dashed), same
     * as the existing design already relies on. */
    g_series = lv_chart_add_series(g_chart, bgr_fix(lv_color_make(0, 0, 255)), LV_CHART_AXIS_PRIMARY_Y);
    /* Terugleverprijs (2026-10-01) - pure green (0,255,0), same unit/
     * scale as g_series above so it shares the PRIMARY axis (unlike gas,
     * which needs its own scale/unit). green=255 exactly satisfies the
     * fb_ili9486 scramble-safety rule on its own, no bgr_fix() swap risk
     * either way (red=blue=0). */
    g_export_series = lv_chart_add_series(g_chart, lv_color_make(0, 255, 0), LV_CHART_AXIS_PRIMARY_Y);
    /* Pure-ish orange (255,152,0), blue=0 route to the same
     * fb_ili9486-scramble safety rule - happens to equal LV_PALETTE_ORANGE
     * exactly (the one palette color in this whole app that was already
     * safe, see screen_solar.c). On the SECONDARY axis - see
     * rescale_gas_axis() for why its range depends on the configured
     * unit. */
    g_gas_series = lv_chart_add_series(g_chart, bgr_fix(lv_color_make(255, 152, 0)), LV_CHART_AXIS_SECONDARY_Y);

    /* Vertical-only marker at the current time — pure indicator of "this
     * is now", not an average/threshold line (see task spec). Blue +
     * dashed (LV_PART_CURSOR is a real styleable chart part - the color
     * param above only sets line_dsc.color, dash width/gap come from the
     * object's own style for that part, see lv_chart.c's draw_cursors()).
     * Pure blue (0,0,255): green=0 exactly satisfies fb_ili9486's
     * BGR-wiring-quirk safety rule (see CLAUDE.md) - the previous
     * LV_PALETTE_RED choice (~244,67,54) violated all three safety rules
     * at once (blue>0, green mid-range, red!=blue), which is exactly why
     * it rendered as purple/pink instead of red (live-reported). */
    g_now_cursor = lv_chart_add_cursor(g_chart, bgr_fix(lv_color_make(0, 0, 255)), LV_DIR_VER);
    lv_obj_set_style_line_dash_width(g_chart, UI_S(6), LV_PART_CURSOR);
    lv_obj_set_style_line_dash_gap(g_chart, UI_S(4), LV_PART_CURSOR);

    /* Fixed vertical marker at kwartier 96 (middernacht, start van
     * "morgen") - zelfde idee/stijl als screen_solar.c/screen_battery.c's
     * eigen dag-grens-cursor voor hun 192-kwartier 2-daagse charts.
     * Positie is vast (kwartier 96 verandert nooit), dus eenmalig hier
     * gezet i.p.v. elke screen_price_update()-tick. */
    g_day_boundary_cursor = lv_chart_add_cursor(g_chart, lv_color_white(), LV_DIR_VER);
    lv_chart_set_cursor_point(g_chart, g_day_boundary_cursor, NULL, IPC_QUARTERS_PER_DAY);

    /* 48u-span (vandaag+morgen) - een echte 00:00-24:00-tijdas per dag,
     * plus een "Vandaag"/"Morgen"-koptekst daaronder (16px eronder, zelfde
     * relatieve afstand als screen_battery.c's versie - screen_solar.c's
     * eigen kopie hergebruikte per ongeluk een vaste y=214 die niet meeschaalt
     * met een andere CHART_HEIGHT, hier bewust relatief t.o.v. CHART_Y/
     * CHART_HEIGHT gehouden). */
    static const char * const day_axis_marks[] = {"00:00", "12:00", "24:00"};
    screen_common_add_time_axis_labels_n(parent, CHART_X, CHART_WIDTH / 2, CHART_Y + CHART_HEIGHT + UI_S(2),
                                          day_axis_marks, 3);
    screen_common_add_time_axis_labels_n(parent, CHART_X + CHART_WIDTH / 2, CHART_WIDTH / 2,
                                          CHART_Y + CHART_HEIGHT + UI_S(2), day_axis_marks, 3);

    lv_obj_t * today_label = lv_label_create(parent);
    lv_label_set_text_static(today_label, "Vandaag");
    lv_obj_set_style_text_font(today_label, &lv_font_signage_14, 0);
    lv_obj_update_layout(today_label);
    lv_obj_set_pos(today_label, CHART_X + CHART_WIDTH / 4 - lv_obj_get_width(today_label) / 2,
                    CHART_Y + CHART_HEIGHT + UI_S(18));

    lv_obj_t * tomorrow_label = lv_label_create(parent);
    lv_label_set_text_static(tomorrow_label, "Morgen");
    lv_obj_set_style_text_font(tomorrow_label, &lv_font_signage_14, 0);
    lv_obj_update_layout(tomorrow_label);
    lv_obj_set_pos(tomorrow_label, CHART_X + (CHART_WIDTH * 3) / 4 - lv_obj_get_width(tomorrow_label) / 2,
                    CHART_Y + CHART_HEIGHT + UI_S(18));

    screen_common_add_y_axis_labels(parent, CHART_X, CHART_Y, CHART_HEIGHT, AXIS_MIN_MEUR, AXIS_MAX_MEUR, 1000,
                                     "EUR/kWh", g_axis_labels);
    screen_common_add_y_axis_labels_right(parent, CHART_RIGHT_X, CHART_Y, CHART_HEIGHT, AXIS_MIN_MEUR,
                                           GAS_AXIS_MAX_MEUR_KWH, 1000, "EUR/kWh", g_gas_axis_labels,
                                           &g_gas_unit_label);
    for(int i = 0; i < 3; i++) lv_obj_set_hidden(g_gas_axis_labels[i], true);
    lv_obj_set_hidden(g_gas_unit_label, true);

    screen_common_add_chart_tooltip(g_chart, N_QUARTERS, price_tooltip_fill, NULL);

    g_empty_label = screen_common_add_empty_state_label(parent, g_chart, "Geen prijsdata");
}

void screen_price_update(const dashboard_state_t * state)
{
    bool any_valid = false;
    for(int i = 0; i < N_QUARTERS; i++) {
        g_price_curve_valid[i] = state->price_curve_quarterly_valid[i];
        g_export_price_curve_valid[i] = state->export_price_curve_quarterly_valid[i];
        if(g_price_curve_valid[i] || g_export_price_curve_valid[i]) any_valid = true;
    }

    double axis_min_eur, axis_max_eur;
    if(compute_price_axis_range(state, &axis_min_eur, &axis_max_eur) &&
       (axis_min_eur != g_axis_applied_min_eur || axis_max_eur != g_axis_applied_max_eur)) {
        int32_t axis_min = (int32_t) lround(axis_min_eur * 1000.0);
        int32_t axis_max = (int32_t) lround(axis_max_eur * 1000.0);
        lv_chart_set_axis_range(g_chart, LV_CHART_AXIS_PRIMARY_Y, axis_min, axis_max);
        screen_common_set_axis_label_value(g_axis_labels[0], axis_max, 1000);
        screen_common_set_axis_label_value(g_axis_labels[1], (axis_min + axis_max) / 2, 1000);
        screen_common_set_axis_label_value(g_axis_labels[2], axis_min, 1000);
        g_axis_applied_min_eur = axis_min_eur;
        g_axis_applied_max_eur = axis_max_eur;
    }

    screen_common_set_hidden(g_empty_label, any_valid);
    screen_common_set_hidden(g_chart, !any_valid);
    /* Legend covers 2 always-on series (inkoop/teruglevering) now, not
     * just gas - visible whenever there's any price data to show at all. */
    lv_obj_set_hidden(g_legend, !any_valid);

    g_gas_visible = state->gas_enabled && state->gas_price_curve_valid;
    for(int i = 0; i < 3; i++) lv_obj_set_hidden(g_gas_axis_labels[i], !g_gas_visible);
    lv_obj_set_hidden(g_gas_unit_label, !g_gas_visible);

    if((g_gas_visible ? 1 : 0) != g_legend_gas_applied) {
        if(g_gas_visible) {
            lv_label_set_text_static(g_legend, "#0000ff inkoop# / #00ff00 teruglevering# / #ff9800 gas#");
        }
        else {
            lv_label_set_text_static(g_legend, "#0000ff inkoop# / #00ff00 teruglevering#");
        }
        g_legend_gas_applied = g_gas_visible ? 1 : 0;
    }

    /* gas_price_curve stays hourly (24, today only - geen "morgen"-fetch,
     * zie price_client.py) in het IPC-bericht - hier uitgerold over alle
     * N_QUARTERS (192) punten door de 24 uurwaarden te herhalen (modulo
     * 24, dus morgen krijgt dezelfde 24 waarden als vandaag) zodat de
     * reeks hetzelfde point_count als de andere twee (nu 2-daagse
     * kwartier-resolutie) reeksen deelt. */
    if(g_gas_visible) {
        snprintf(g_gas_unit, sizeof(g_gas_unit), "%s", state->gas_price_unit);
        if(strcmp(g_gas_unit_applied, g_gas_unit) != 0) rescale_gas_axis(g_gas_unit);

        int32_t gas_values[N_QUARTERS];
        for(int i = 0; i < N_QUARTERS; i++) {
            double hourly_value = state->gas_price_curve[(i / 4) % IPC_PRICE_CURVE_LEN];
            gas_values[i] = (int32_t) round(hourly_value * 1000.0);
            g_gas_curve[i] = hourly_value;
        }
        lv_chart_set_series_values(g_chart, g_gas_series, gas_values, N_QUARTERS);
    }
    else {
        int32_t none_values[N_QUARTERS];
        for(int i = 0; i < N_QUARTERS; i++) none_values[i] = LV_CHART_POINT_NONE;
        lv_chart_set_series_values(g_chart, g_gas_series, none_values, N_QUARTERS);
    }

    if(!any_valid) {
        return;
    }

    int32_t values[N_QUARTERS];
    for(int i = 0; i < N_QUARTERS; i++) {
        values[i] = g_price_curve_valid[i] ? (int32_t) round(state->price_curve_quarterly[i] * 1000.0)
                                            : LV_CHART_POINT_NONE;
        g_price_curve[i] = state->price_curve_quarterly[i];
    }
    lv_chart_set_series_values(g_chart, g_series, values, N_QUARTERS);

    int32_t export_values[N_QUARTERS];
    for(int i = 0; i < N_QUARTERS; i++) {
        export_values[i] = g_export_price_curve_valid[i]
                                ? (int32_t) round(state->export_price_curve_quarterly[i] * 1000.0)
                                : LV_CHART_POINT_NONE;
        g_export_price_curve[i] = state->export_price_curve_quarterly[i];
    }
    lv_chart_set_series_values(g_chart, g_export_series, export_values, N_QUARTERS);

    time_t now = time(NULL);
    struct tm local_tm;
    localtime_r(&now, &local_tm);
    uint32_t quarter = (uint32_t) ((local_tm.tm_hour * 60 + local_tm.tm_min) / 15);
    lv_chart_set_cursor_point(g_chart, g_now_cursor, g_series, quarter);
}
