#include "screen_consumption.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "screen_common.h"
#include "fonts_signage.h"

/* IPC's expected_consumption_curve/actual_consumption_curve are energy
 * (kWh) per 15-min kwartier, but this screen displays average power (kW)
 * instead (2026-10-05, on request) - kW = kWh / 0.25h = kWh * 4, applied
 * at every point a curve value is read below (compute_axis_max_kw(),
 * fill_series_values(), the tooltip cache in screen_consumption_update()).
 * The underlying IPC field/unit itself is unchanged - this is a display-
 * only conversion, same "convert once at the render boundary" approach as
 * screen_battery.c's identical Wh-per-kwartier -> kW change. */
#define KWH_PER_KWARTIER_TO_KW 4.0

#define AXIS_MIN_W 0    /* W, i.e. kW * 1000 - see the chart-internal-units comment on fill_series_values() */
#define AXIS_MAX_W_DEFAULT 1500 /* placeholder until the first real curve rescales this, see rescale_axis() below */
/* Rounding step (kW) for the dynamic rescale - 2026-10-01,
 * Jeroen: this fixed 1500W/1.5kW ceiling was already clipping real data
 * (observed up to ~2.3kWh/kwartier, i.e. ~9.2kW average - same "vaste
 * aanname, niet afgeleid van echte klantdata" gap CLAUDE.md already
 * flagged), fixed the same way as screen_price.c's
 * PRICE_AXIS_ROUND_STEP_EUR. Value unchanged by the kWh->kW display
 * conversion (2026-10-05) - 0.1kW steps are still a sensible resolution
 * now that the axis itself reads up to ~9kW instead of ~2.3kWh. */
#define AXIS_ROUND_STEP_KW 0.1

/* ESP32 (800x480): op de Pi letterlijk 56/46/414/206 in de aanroepen hieronder.
 * Linkermarge (as-labels) schaalt met de lettergrootte, de grafiek vult de rest
 * (zelfde 10px rechtermarge, x1,5). */
#define CHART_X UI_S(56)
#define CHART_Y UI_SY(46)
#define CHART_WIDTH (UI_SCREEN_W - CHART_X - UI_S(10))
#define CHART_HEIGHT UI_SY(206)

static lv_obj_t * g_chart;
static lv_chart_series_t * g_series;         /* expected (verwacht) */
static lv_chart_series_t * g_actual_series;  /* actual (werkelijk), added 2026-09-27 */
static lv_chart_cursor_t * g_now_cursor;
static lv_obj_t * g_empty_label;
static lv_obj_t * g_axis_labels[3]; /* [max, mid, min], see screen_common_add_y_axis_labels() */
/* NAN sentinel, same reasoning as screen_solar.c's g_axis_applied_for_kwp -
 * the very first real max (including a legitimate exact match to
 * AXIS_MAX_W_DEFAULT) must still trigger the initial rescale/relabel. */
static double g_axis_applied_max_kw = NAN;

/* Cached from the last screen_consumption_update() call, for the
 * tap-tooltip (added 2026-09-27) - see screen_price.c's identical-purpose
 * cache for why this can't just read from `state` at tap time. Already in
 * kW (post KWH_PER_KWARTIER_TO_KW conversion), not the raw IPC kWh. */
static bool g_expected_valid[IPC_QUARTERS_PER_DAY];
static double g_expected_curve[IPC_QUARTERS_PER_DAY];
static bool g_actual_valid[IPC_QUARTERS_PER_DAY];
static double g_actual_curve[IPC_QUARTERS_PER_DAY];

/* Max across both curves' *valid* entries, converted to kW and rounded up
 * to AXIS_ROUND_STEP_KW steps - mirrors screen_price.c's
 * compute_price_axis_range() idea, just max-only (no negative-value case
 * here, unlike EPEX prices). Returns false (no valid data) if neither
 * curve has a single valid entry - caller keeps whatever was last
 * applied in that case. */
static bool compute_axis_max_kw(const dashboard_state_t * state, double * out_max_kw)
{
    double hi = 0.0;
    bool any = false;
    for(int i = 0; i < IPC_QUARTERS_PER_DAY; i++) {
        if(state->expected_consumption_curve_valid[i]) {
            double v = state->expected_consumption_curve[i] * KWH_PER_KWARTIER_TO_KW;
            if(!any || v > hi) hi = v;
            any = true;
        }
        if(state->actual_consumption_curve_valid[i]) {
            double v = state->actual_consumption_curve[i] * KWH_PER_KWARTIER_TO_KW;
            if(!any || v > hi) hi = v;
            any = true;
        }
    }
    if(!any) return false;

    double rounded = ceil(hi / AXIS_ROUND_STEP_KW) * AXIS_ROUND_STEP_KW;
    *out_max_kw = (rounded > 0.0) ? rounded : AXIS_ROUND_STEP_KW;
    return true;
}

static void consumption_tooltip_fill(void * user_data, int index, char * buf, size_t buf_size)
{
    (void) user_data;
    int hour = (index * 15) / 60;
    int minute = (index * 15) % 60;

    char expected_buf[24];
    if(g_expected_valid[index]) snprintf(expected_buf, sizeof(expected_buf), "%.2f kW", g_expected_curve[index]);
    else snprintf(expected_buf, sizeof(expected_buf), "geen data");

    char actual_buf[24];
    if(g_actual_valid[index]) snprintf(actual_buf, sizeof(actual_buf), "%.2f kW", g_actual_curve[index]);
    else snprintf(actual_buf, sizeof(actual_buf), "geen data");

    snprintf(buf, buf_size, "%02d:%02d\nVerwacht: %s\nWerkelijk: %s", hour, minute, expected_buf, actual_buf);
}

void screen_consumption_build(lv_obj_t * parent)
{
    lv_obj_t * title = lv_label_create(parent);
    lv_label_set_text_static(title, "Verwacht verbruik (kW)");
    lv_obj_set_pos(title, UI_SX(10), UI_SY(4));
    lv_obj_set_style_text_font(title, &lv_font_signage_18, 0);

    /* Small legend row between title and chart, distinguishing the two
     * series added below - hex values are R/B-swapped (BGR-wiring quirk,
     * same as bgr_fix() but recolor tags take raw hex text) versions of
     * the series colors: light green (0,255,0)->00ff00, dark green
     * (0,100,0)->006400. */
    lv_obj_t * legend = lv_label_create(parent);
    lv_label_set_recolor(legend, true);
    lv_label_set_text_static(legend, "#00ff00 verwacht# / #006400 werkelijk#");
    lv_obj_set_style_text_font(legend, &lv_font_signage_14, 0);
    lv_obj_set_pos(legend, UI_SX(10), UI_SY(26));

    /* Chart shifted down 20px for the legend row above, height reduced by
     * the same 20px so the bottom edge (and the time-axis position below
     * it) stays exactly where it was. x=56/width=414 (not the usual
     * 10/460) - leaves room on the left for
     * screen_common_add_y_axis_labels()'s rotated kW unit tag + number
     * column, same as screen_price.c/screen_solar.c. */
    g_chart = lv_chart_create(parent);
    lv_obj_set_pos(g_chart, CHART_X, CHART_Y);
    lv_obj_set_size(g_chart, CHART_WIDTH, CHART_HEIGHT);
    lv_chart_set_type(g_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(g_chart, IPC_QUARTERS_PER_DAY);
    lv_chart_set_axis_range(g_chart, LV_CHART_AXIS_PRIMARY_Y, AXIS_MIN_W, AXIS_MAX_W_DEFAULT);

    /* Pure green (0,255,0), not LV_PALETTE_GREEN (76,175,80) - the latter
     * fails all three fb_ili9486 scramble-safety conditions (see
     * screen_price.c's cursor comment / CLAUDE.md "Scherm" section) and
     * rendered wrong on real hardware; green=255 is one of the three safe
     * escapes. Same green COLOR_SUCCESS already settled on for this exact
     * reason on the Python/Pillow onboarding screens. */
    g_series = lv_chart_add_series(g_chart, bgr_fix(lv_color_make(0, 255, 0)), LV_CHART_AXIS_PRIMARY_Y);
    /* Dark green (0,100,0) - same color (and same scramble-safety route,
     * blue=0) as the header's hamburger/gear buttons, on request: light
     * green = verwacht, dark green = werkelijk. */
    g_actual_series = lv_chart_add_series(g_chart, bgr_fix(lv_color_make(0, 100, 0)), LV_CHART_AXIS_PRIMARY_Y);
    /* Blue + dashed "now" marker - see screen_price.c's build function for
     * why (LV_PALETTE_RED rendered purple/pink on this BGR-wired panel). */
    g_now_cursor = lv_chart_add_cursor(g_chart, bgr_fix(lv_color_make(0, 0, 255)), LV_DIR_VER);
    lv_obj_set_style_line_dash_width(g_chart, UI_S(6), LV_PART_CURSOR);
    lv_obj_set_style_line_dash_gap(g_chart, UI_S(4), LV_PART_CURSOR);

    screen_common_add_time_axis_labels(parent, CHART_X, CHART_WIDTH, CHART_Y + CHART_HEIGHT + UI_S(2));
    screen_common_add_y_axis_labels(parent, CHART_X, CHART_Y, CHART_HEIGHT, AXIS_MIN_W, AXIS_MAX_W_DEFAULT, 1000, "kW", g_axis_labels);
    screen_common_add_chart_tooltip(g_chart, IPC_QUARTERS_PER_DAY, consumption_tooltip_fill, NULL);

    g_empty_label = screen_common_add_empty_state_label(
        parent, g_chart, "Onvoldoende data\n(nog geen 14 dagen geschiedenis)");
    lv_obj_set_style_text_align(g_empty_label, LV_TEXT_ALIGN_CENTER, 0);
}

/* Shared by both series - LV_CHART_POINT_NONE marks a quarter with no data
 * (expected: too little history; actual: hasn't happened yet today, or a
 * gap during an outage) so only the quarters with real data draw.
 * Converts the IPC curve's kWh-per-kwartier to kW (see
 * KWH_PER_KWARTIER_TO_KW) before the existing *1000 chart-internal-units
 * scaling. */
static void fill_series_values(int32_t values[IPC_QUARTERS_PER_DAY], const bool valid[IPC_QUARTERS_PER_DAY],
                                const double curve[IPC_QUARTERS_PER_DAY])
{
    for(int i = 0; i < IPC_QUARTERS_PER_DAY; i++) {
        values[i] = valid[i] ? (int32_t) round(curve[i] * KWH_PER_KWARTIER_TO_KW * 1000.0) : LV_CHART_POINT_NONE;
    }
}

/* Converts+copies an IPC kWh-per-kwartier curve into a kW cache array, for
 * the tap-tooltip (which must show the exact same kW number as the
 * plotted line/axis, see consumption_tooltip_fill()). A loop, not memcpy
 * like before the kWh->kW display conversion - the values themselves now
 * differ from the IPC source, not just a type-compatible raw copy. */
static void cache_curve_kw(double cache[IPC_QUARTERS_PER_DAY], const double curve[IPC_QUARTERS_PER_DAY])
{
    for(int i = 0; i < IPC_QUARTERS_PER_DAY; i++) {
        cache[i] = curve[i] * KWH_PER_KWARTIER_TO_KW;
    }
}

void screen_consumption_update(const dashboard_state_t * state)
{
    bool any_valid = false;
    for(int i = 0; i < IPC_QUARTERS_PER_DAY; i++) {
        if(state->expected_consumption_curve_valid[i] || state->actual_consumption_curve_valid[i]) {
            any_valid = true;
            break;
        }
    }

    double axis_max_kw;
    if(compute_axis_max_kw(state, &axis_max_kw) && axis_max_kw != g_axis_applied_max_kw) {
        int32_t axis_max = (int32_t) lround(axis_max_kw * 1000.0);
        lv_chart_set_axis_range(g_chart, LV_CHART_AXIS_PRIMARY_Y, AXIS_MIN_W, axis_max);
        screen_common_set_axis_label_value(g_axis_labels[0], axis_max, 1000);
        screen_common_set_axis_label_value(g_axis_labels[1], axis_max / 2, 1000);
        screen_common_set_axis_label_value(g_axis_labels[2], AXIS_MIN_W, 1000);
        g_axis_applied_max_kw = axis_max_kw;
    }

    screen_common_set_hidden(g_empty_label, any_valid);
    screen_common_set_hidden(g_chart, !any_valid);
    if(!any_valid) {
        /* Chart (and any tooltip on it) is hidden in this branch, but
         * clear the cache anyway so a stale tap-tooltip doesn't show old
         * data if this screen somehow becomes visible again before a
         * fresh valid update arrives. */
        for(int i = 0; i < IPC_QUARTERS_PER_DAY; i++) {
            g_expected_valid[i] = false;
            g_actual_valid[i] = false;
        }
        return;
    }

    int32_t values[IPC_QUARTERS_PER_DAY];
    fill_series_values(values, state->expected_consumption_curve_valid, state->expected_consumption_curve);
    lv_chart_set_series_values(g_chart, g_series, values, IPC_QUARTERS_PER_DAY);
    memcpy(g_expected_valid, state->expected_consumption_curve_valid, sizeof(g_expected_valid));
    cache_curve_kw(g_expected_curve, state->expected_consumption_curve);

    fill_series_values(values, state->actual_consumption_curve_valid, state->actual_consumption_curve);
    lv_chart_set_series_values(g_chart, g_actual_series, values, IPC_QUARTERS_PER_DAY);
    memcpy(g_actual_valid, state->actual_consumption_curve_valid, sizeof(g_actual_valid));
    cache_curve_kw(g_actual_curve, state->actual_consumption_curve);

    time_t now = time(NULL);
    struct tm local_tm;
    localtime_r(&now, &local_tm);
    uint32_t quarter = (uint32_t) ((local_tm.tm_hour * 60 + local_tm.tm_min) / 15);
    lv_chart_set_cursor_point(g_chart, g_now_cursor, g_series, quarter);
}
