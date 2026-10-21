#include "screen_battery.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "screen_common.h"
#include "fonts_signage.h"

#define N_QUARTERS IPC_BATTERY_PLAN_LEN

/* Placeholder W-axis range until the first real screen_battery_update()
 * rescales it to the actual eigen_zon/actief_gestuurd/verwacht_verbruik/
 * geblokkeerd data (see compute_power_axis_range_w() below). Used to be
 * tied to battery_capacity_kwh (the full ~5.12 kWh battery capacity) -
 * changed 2026-10-02 (Jeroen: "ik wil dat de batterijlading alleen de SOC
 * lijn volgt, dus niet langer dat SOC ook kWh laat zien, daardoor kan je
 * beter de zonverwachting en het verwachte verbruik zien") - these series
 * are per-kwartier energy amounts (max ~625 Wh at 2500W/kwartier), nowhere
 * near a full battery's capacity, so scaling this axis to capacity
 * squashed them all flat near the bottom. SOC now ONLY ever appears as a
 * percentage on its own SECONDARY_Y axis below - battery_capacity_kwh is
 * no longer read by this screen at all.
 *
 * Displayed as average power (kW), not energy (kWh), since 2026-10-05 (on
 * request) - the IPC fields themselves are still Wh-per-kwartier (energy),
 * converted at the render boundary: W = Wh-per-kwartier / 0.25h =
 * Wh-per-kwartier * 4 (see WH_PER_KWARTIER_TO_W, applied in
 * fill_series_values()'s scale argument and compute_power_axis_range_w()).
 * This conveniently recovers the real instantaneous power level (e.g. a
 * steady 2500W charge command shows as 2500W/2.5kW here, not the less
 * intuitive "625 Wh this kwartier"). */
#define WH_PER_KWARTIER_TO_W 4.0
#define POWER_AXIS_MIN_W 0
#define AXIS_MAX_W_DEFAULT 1000
#define POWER_AXIS_ROUND_STEP_W 200

/* SOC (%) secondary right-hand axis - fixed 0-100 range, never rescaled.
 * Matches dashboard-web/static/app.js's Batterij chart rightAxis - both
 * dashboards must show this identically (2026-10-01, Jeroen: "het web
 * dashboard en het EMS dashboard moet exact overeenkomen"). Chart
 * narrowed (CHART_WIDTH below, was a plain 414) to make room for this
 * axis's number column + rotated unit label on the right, same layout
 * trick screen_price.c already uses for its secondary-Y gas-price axis. */
#define SOC_AXIS_MIN 0
#define SOC_AXIS_MAX 100
/* ESP32 (800x480): zie screen_price.c - marges x1,5 (rechts ruimte voor de
 * SOC-as), grafiek vult de rest van de breedte. */
#define CHART_X UI_S(56)
/* Was 34 (straddles the top gridline, -10 nudge below the old title row -
 * see screen_common.c) - now 10, flush with the other screens' usual top
 * margin, since screen_battery_build() no longer draws a "Batterijgedrag"
 * title (removed 2026-10-15, Jeroen: wanted the freed space for a bigger
 * legend font instead). Every y-position below this that was hand-tuned
 * relative to the old CHART_Y (time-axis labels, which are computed from
 * CHART_Y+CHART_HEIGHT and shift automatically; Vandaag/Morgen; the
 * legend) is adjusted by the same -24px delta. */
#define CHART_Y UI_SY(10)
#define CHART_WIDTH (UI_SCREEN_W - CHART_X - UI_S(60))
#define CHART_HEIGHT UI_SY(162)
#define CHART_RIGHT_X (CHART_X + CHART_WIDTH)

static lv_obj_t * g_line_chart;
static lv_chart_series_t * g_eigen_zon_series;
static lv_chart_series_t * g_actief_gestuurd_series;
static lv_chart_series_t * g_verwacht_verbruik_series;
static lv_chart_series_t * g_geblokkeerd_series;
static lv_chart_series_t * g_batterijlading_series;
static lv_obj_t * g_axis_labels[3]; /* [max, mid, min], see screen_common_add_y_axis_labels() */
static lv_obj_t * g_soc_axis_labels[3]; /* [max, mid, min], see screen_common_add_y_axis_labels_right() */
static lv_obj_t * g_empty_label;
/* NAN (not 0) so the very first real computed range - including a
 * hypothetical legitimate 0 - is never mistaken for "already applied,
 * skip" (same reasoning/pattern as screen_price.c's
 * g_axis_applied_min/max_eur). */
static double g_axis_applied_min_w = NAN;
static double g_axis_applied_max_w = NAN;

/* Cached from the last screen_battery_update() call, for the tap-tooltip -
 * same reasoning as screen_consumption.c's identical-purpose caches (the
 * tooltip callback fires from a tap event, not from an _update() call, so
 * it can't read dashboard_state_t directly). Already converted to W (see
 * WH_PER_KWARTIER_TO_W), not the raw IPC Wh-per-kwartier. */
static bool g_eigen_zon_valid[N_QUARTERS];
static double g_eigen_zon_w[N_QUARTERS];
static bool g_actief_gestuurd_valid[N_QUARTERS];
static double g_actief_gestuurd_w[N_QUARTERS];
static bool g_verwacht_verbruik_valid[N_QUARTERS];
static double g_verwacht_verbruik_w[N_QUARTERS];
static bool g_geblokkeerd_valid[N_QUARTERS];
static double g_geblokkeerd_w[N_QUARTERS];
static bool g_soc_valid[N_QUARTERS];
static double g_soc_pct[N_QUARTERS];

static void battery_tooltip_fill(void * user_data, int index, char * buf, size_t buf_size)
{
    (void) user_data;

    char zon_buf[16];
    if(!g_eigen_zon_valid[index]) snprintf(zon_buf, sizeof(zon_buf), "-");
    else snprintf(zon_buf, sizeof(zon_buf), "%.2f kW", g_eigen_zon_w[index] / 1000.0);

    char actief_buf[16];
    if(!g_actief_gestuurd_valid[index]) snprintf(actief_buf, sizeof(actief_buf), "-");
    else snprintf(actief_buf, sizeof(actief_buf), "%.2f kW", g_actief_gestuurd_w[index] / 1000.0);

    char verbruik_buf[16];
    if(!g_verwacht_verbruik_valid[index]) snprintf(verbruik_buf, sizeof(verbruik_buf), "-");
    else snprintf(verbruik_buf, sizeof(verbruik_buf), "%.2f kW", g_verwacht_verbruik_w[index] / 1000.0);

    char geblokkeerd_buf[16];
    if(!g_geblokkeerd_valid[index]) snprintf(geblokkeerd_buf, sizeof(geblokkeerd_buf), "-");
    else snprintf(geblokkeerd_buf, sizeof(geblokkeerd_buf), "%.2f kW", g_geblokkeerd_w[index] / 1000.0);

    /* Percentage only (2026-10-02) - no longer converted to kWh, see
     * AXIS_MAX_W_DEFAULT's comment above for why. */
    char lading_buf[16];
    if(!g_soc_valid[index]) snprintf(lading_buf, sizeof(lading_buf), "-");
    else snprintf(lading_buf, sizeof(lading_buf), "%.0f%%", g_soc_pct[index]);

    /* index 0-95 = vandaag, 96-191 = morgen (2026-10-02: switched from 48
     * hourly buckets to 192 kwartier buckets, 2 days still) - a bare
     * "%02d:00" would silently repeat 00:00-23:45 twice with no way to
     * tell which day a tapped point belongs to. */
    int quarter_of_day = index % IPC_QUARTERS_PER_DAY;
    int hour = (quarter_of_day * 15) / 60;
    int minute = (quarter_of_day * 15) % 60;
    snprintf(buf, buf_size,
             "%s%02d:%02d\nZonoverschot: %s\nActief laden: %s\nVerwacht verbruik: %s\nGeblokkeerd: %s\nBatterijlading: %s",
             index >= IPC_QUARTERS_PER_DAY ? "Morgen " : "", hour, minute, zon_buf, actief_buf, verbruik_buf,
             geblokkeerd_buf, lading_buf);
}

void screen_battery_build(lv_obj_t * parent)
{
    /* No title here (removed 2026-10-15, see CHART_Y's comment) - unlike
     * the other screens, which all keep their "Kosten"/"Verbruik"/"Zon"
     * title row. */

    /* x=56/width=414 (not the usual 10/460) - leaves room on the left for
     * screen_common_add_y_axis_labels()'s rotated kW unit tag + number
     * column, same as screen_price.c/screen_solar.c/screen_consumption.c.
     *
     * Single LINE chart for all 4 series (2026-10-02) - was a BAR chart
     * (eigen_zon/actief_gestuurd) overlaid with a transparent LINE chart
     * (verwacht_verbruik/SOC) until the 48->192-point kwartier switch:
     * 192 bar columns across CHART_WIDTH (364px) is under 2px/column
     * before even splitting that between 2 series, which rendered on real
     * hardware as a single dark smeared band, not readable bars (live-
     * reported by Jeroen, not caught in dev since the web dashboard
     * already drew everything as lines and never hit this). Lines don't
     * have a per-column width floor the way bars do, so this resolves at
     * any point count - matches dashboard-web/static/app.js, which has
     * always drawn eigen_zon/actief_gestuurd as lines. */
    g_line_chart = lv_chart_create(parent);
    lv_obj_set_pos(g_line_chart, CHART_X, CHART_Y);
    lv_obj_set_size(g_line_chart, CHART_WIDTH, CHART_HEIGHT);
    lv_chart_set_type(g_line_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(g_line_chart, N_QUARTERS);
    lv_chart_set_axis_range(g_line_chart, LV_CHART_AXIS_PRIMARY_Y, POWER_AXIS_MIN_W, AXIS_MAX_W_DEFAULT);
    lv_chart_set_axis_range(g_line_chart, LV_CHART_AXIS_SECONDARY_Y, SOC_AXIS_MIN, SOC_AXIS_MAX);

    /* None of these 4 series use lv_palette_main() anymore - both
     * LVGL palette colors originally tried here (amber/purple) failed the
     * fb_ili9486 scramble-safety check (see screen_price.c's cursor
     * comment / CLAUDE.md "Scherm" section), checked against the logical
     * pre-bgr_fix color: green must be exactly 0/255, or blue exactly 0,
     * or red exactly equal blue. Each color below is a minimally-adjusted,
     * safe substitute for the original palette color (named in each
     * comment) - the legend's recolor hex tags further down match. */
    g_eigen_zon_series =
        lv_chart_add_series(g_line_chart, bgr_fix(lv_color_make(255, 193, 0)), LV_CHART_AXIS_PRIMARY_Y); /* was AMBER */
    g_actief_gestuurd_series = lv_chart_add_series(
        g_line_chart, bgr_fix(lv_color_make(166, 39, 166)), LV_CHART_AXIS_PRIMARY_Y); /* was PURPLE */
    g_verwacht_verbruik_series = lv_chart_add_series(g_line_chart, bgr_fix(lv_color_make(118, 125, 118)),
                                                       LV_CHART_AXIS_PRIMARY_Y); /* was BLUE_GREY */
    /* Puur rood (255,0,0) - blauw=0 is altijd scramble-veilig (zie de
     * fb_ili9486-kleurenbug-regel hierboven bij de andere 3 series), geen
     * aangepaste tint nodig zoals bij amber/purple. Nieuw 2026-10-15: hoe
     * veel zelfconsumptie-ontladen dit kwartier actief geblokkeerd is. */
    g_geblokkeerd_series =
        lv_chart_add_series(g_line_chart, bgr_fix(lv_color_make(255, 0, 0)), LV_CHART_AXIS_PRIMARY_Y);
    /* SECONDARY_Y, not PRIMARY_Y - SOC gets its own fixed 0-100% axis
     * (see SOC_AXIS_MIN/MAX above), independent of the Wh axis the other
     * 3 series share. */
    g_batterijlading_series = lv_chart_add_series(g_line_chart, bgr_fix(lv_color_make(100, 255, 100)),
                                                    LV_CHART_AXIS_SECONDARY_Y); /* was GREEN */

    /* 48h span (vandaag+morgen, 192 kwartierpunten sinds 2026-10-02) - two
     * rows below the chart: an actual 00:00-24:00 time axis for EACH day
     * separately (2026-10-02, Jeroen: "de x as van de batterij is
     * onoverzichtelijk, ik wil dat je gewoon van 0:00 tot 23:59 voor beide
     * dagen laat zien" - a single continuous 48h axis with only
     * "Vandaag"/"Morgen" text gave no time-of-day reference at all), plus
     * the existing "Vandaag"/"Morgen" day header underneath that. Each
     * half gets its own independent screen_common_add_time_axis_labels_n()
     * call (chart_width/2, not the full width) so "00:00"/"24:00" land at
     * that day's own edges, not the whole chart's. Only 3 marks per half
     * (not the usual 5) - half the width per label vs. a single-day chart,
     * and fewer marks is exactly what was asked for ("onoverzichtelijk"). */
    static const char * const day_axis_marks[] = {"00:00", "12:00", "24:00"};
    screen_common_add_time_axis_labels_n(parent, CHART_X, CHART_WIDTH / 2, CHART_Y + CHART_HEIGHT + UI_S(2),
                                          day_axis_marks, 3);
    screen_common_add_time_axis_labels_n(parent, CHART_X + CHART_WIDTH / 2, CHART_WIDTH / 2,
                                          CHART_Y + CHART_HEIGHT + UI_S(2), day_axis_marks, 3);

    /* Not using screen_common_add_time_axis_labels_n() for these two - that
     * helper spreads N marks evenly edge-to-edge (fraction i/(n-1)), which
     * would place a 2-entry {"Vandaag","Morgen"} pair at the two edges
     * rather than centered under each day's own half of the chart.
     * Positioned manually instead: each label centered under its
     * respective 24h half (chart_width/4 and 3*chart_width/4). */
    lv_obj_t * today_label = lv_label_create(parent);
    lv_label_set_text_static(today_label, "Vandaag");
    lv_obj_set_style_text_font(today_label, &lv_font_signage_14, 0);
    lv_obj_update_layout(today_label);
    lv_obj_set_pos(today_label, CHART_X + CHART_WIDTH / 4 - lv_obj_get_width(today_label) / 2, CHART_Y + CHART_HEIGHT + UI_S(18));

    lv_obj_t * tomorrow_label = lv_label_create(parent);
    lv_label_set_text_static(tomorrow_label, "Morgen");
    lv_obj_set_style_text_font(tomorrow_label, &lv_font_signage_14, 0);
    lv_obj_update_layout(tomorrow_label);
    lv_obj_set_pos(tomorrow_label, CHART_X + (CHART_WIDTH * 3) / 4 - lv_obj_get_width(tomorrow_label) / 2, CHART_Y + CHART_HEIGHT + UI_S(18));
    screen_common_add_y_axis_labels(parent, CHART_X, CHART_Y, CHART_HEIGHT, POWER_AXIS_MIN_W, AXIS_MAX_W_DEFAULT,
                                     1000, "kW", g_axis_labels);
    screen_common_add_y_axis_labels_right(parent, CHART_RIGHT_X, CHART_Y, CHART_HEIGHT, SOC_AXIS_MIN, SOC_AXIS_MAX,
                                           1, "SOC%", g_soc_axis_labels, NULL);
    screen_common_add_chart_tooltip(g_line_chart, N_QUARTERS, battery_tooltip_fill, NULL);

    g_empty_label = screen_common_add_empty_state_label(
        parent, g_line_chart, "Nog geen planning\n(batterij niet bereikbaar of net gestart)");
    lv_obj_set_style_text_align(g_empty_label, LV_TEXT_ALIGN_CENTER, 0);

    /* Hex values are R/B-swapped (BGR-wiring quirk, same as bgr_fix() but
     * recolor tags take raw hex text) versions of the scramble-safe series
     * colors above, kept in sync with them: gold (255,193,0)->00c1ff,
     * purple (166,39,166)->a627a6, grey (118,125,118)->767d76, rood
     * (255,0,0)->0000ff (blauw=0 blijft blauw=0, alleen R/B wisselen van
     * kanaal), light-green (100,255,100)->64ff64. */
    /* Split over 2 regels (nodig sinds de 5e serie "geblokkeerd",
     * 2026-10-15 - op 1 regel liep de tekst voorbij de 480px-brede
     * schermrand). Font terug naar signage_18 (2026-10-15, Jeroen: titel
     * weg, gebruik de vrijgekomen ruimte om de legenda beter leesbaar te
     * maken) - gemeten met lv_font_get_glyph_width() dat beide regels ruim
     * binnen de 460px beschikbare breedte blijven (344px/271px) en de 2
     * regels (2*20=40px line-height) ruim passen in de ~68px die nu onder
     * de grafiek over is (legend start y=208, tab-inhoud loopt door tot
     * y=276 = 320-HEADER_HEIGHT). */
    lv_obj_t * legend = lv_label_create(parent);
    lv_label_set_recolor(legend, true);
    lv_label_set_text_static(
        legend,
        "#ffc100 zonoverschot# / #a627a6 actief laden# / #767d76 verwacht verbruik#\n"
        "#ff0000 geblokkeerd# / #64ff64 Batterijlading (SOC%)#");
    lv_obj_set_style_text_font(legend, &lv_font_signage_18, 0);
    lv_obj_set_pos(legend, UI_SX(10), CHART_Y + CHART_HEIGHT + UI_S(36));
}

/* Shared by all 4 series - LV_CHART_POINT_NONE marks a kwartier with no
 * plan yet (entirely before "now", see dashboard/battery_scheduler.py's
 * None convention) so only kwartieren with a real plan draw. */
static void fill_series_values(int32_t values[N_QUARTERS], const bool valid[N_QUARTERS], const double source[N_QUARTERS],
                                double scale)
{
    for(int i = 0; i < N_QUARTERS; i++) {
        values[i] = valid[i] ? (int32_t) lround(source[i] * scale) : LV_CHART_POINT_NONE;
    }
}

/* Converts+copies an IPC Wh-per-kwartier curve into a W cache array, for
 * the tap-tooltip (which must show the exact same kW number as the
 * plotted line/axis, see battery_tooltip_fill()). A loop, not memcpy like
 * before the Wh->W display conversion - the values themselves now differ
 * from the IPC source, not just a type-compatible raw copy. */
static void cache_curve_w(double cache[N_QUARTERS], const double curve[N_QUARTERS])
{
    for(int i = 0; i < N_QUARTERS; i++) {
        cache[i] = curve[i] * WH_PER_KWARTIER_TO_W;
    }
}

/* Computes the primary (W) axis range that fits eigen_zon/actief_gestuurd/
 * verwacht_verbruik/geblokkeerd with no clipping, rounded to
 * POWER_AXIS_ROUND_STEP_W steps - mirrors screen_price.c's
 * compute_price_axis_range() exactly (2026-10-02, replacing the old
 * battery-capacity-based fixed range, see AXIS_MAX_W_DEFAULT's comment).
 * Converts the IPC curves' Wh-per-kwartier to W via WH_PER_KWARTIER_TO_W
 * before computing lo/hi, consistent with what fill_series_values()
 * actually plots (2026-10-05). SOC is deliberately NOT included - it
 * lives on its own SECONDARY_Y axis, always 0-100%. Min stays pinned at 0
 * unless the data actually goes negative (shouldn't normally happen for
 * these series, but a known upstream data-quality gap can currently still
 * let verwacht_verbruik dip below 0 - see dashboard/storage.py's
 * expected_consumption()/the pre-2026-09-30 grid_power-vs-household_load
 * consumption_quarterly mixing issue). Returns false (no valid data to
 * scale to) if none of the series have any valid value - caller keeps
 * whatever range was last applied in that case. */
static bool compute_power_axis_range_w(const dashboard_state_t * state, double * out_min_w, double * out_max_w)
{
    double lo = 0.0, hi = 0.0;
    bool any = false;
    const bool * valids[] = {
        state->battery_plan_eigen_zon_wh_valid, state->battery_plan_actief_gestuurd_wh_valid,
        state->battery_plan_verwacht_verbruik_wh_valid, state->battery_plan_geblokkeerd_wh_valid,
    };
    const double * series[] = {
        state->battery_plan_eigen_zon_wh, state->battery_plan_actief_gestuurd_wh,
        state->battery_plan_verwacht_verbruik_wh, state->battery_plan_geblokkeerd_wh,
    };
    for(int s = 0; s < 4; s++) {
        for(int i = 0; i < N_QUARTERS; i++) {
            if(!valids[s][i]) continue;
            double v = series[s][i] * WH_PER_KWARTIER_TO_W;
            if(!any || v < lo) lo = v;
            if(!any || v > hi) hi = v;
            any = true;
        }
    }
    if(!any) return false;

    if(lo > 0.0) lo = 0.0; /* never show a positive-only range as starting above 0 */
    *out_min_w = (lo < 0.0) ? floor(lo / POWER_AXIS_ROUND_STEP_W) * POWER_AXIS_ROUND_STEP_W : 0.0;
    double rounded_max = ceil(hi / POWER_AXIS_ROUND_STEP_W) * POWER_AXIS_ROUND_STEP_W;
    *out_max_w = (rounded_max > *out_min_w) ? rounded_max : *out_min_w + POWER_AXIS_ROUND_STEP_W;
    return true;
}

void screen_battery_update(const dashboard_state_t * state)
{
    /* Rescale the W axis to whatever eigen_zon/actief_gestuurd/
     * verwacht_verbruik actually contain, only re-applying when the
     * computed range actually changed (same pattern as
     * screen_price.c's compute_price_axis_range() use). */
    double axis_min_w, axis_max_w;
    if(compute_power_axis_range_w(state, &axis_min_w, &axis_max_w) &&
       (axis_min_w != g_axis_applied_min_w || axis_max_w != g_axis_applied_max_w)) {
        int32_t axis_min = (int32_t) lround(axis_min_w);
        int32_t axis_max = (int32_t) lround(axis_max_w);
        lv_chart_set_axis_range(g_line_chart, LV_CHART_AXIS_PRIMARY_Y, axis_min, axis_max);
        screen_common_set_axis_label_value(g_axis_labels[0], axis_max, 1000);
        screen_common_set_axis_label_value(g_axis_labels[1], (axis_min + axis_max) / 2, 1000);
        screen_common_set_axis_label_value(g_axis_labels[2], axis_min, 1000);
        g_axis_applied_min_w = axis_min_w;
        g_axis_applied_max_w = axis_max_w;
    }

    bool any_valid = false;
    for(int i = 0; i < N_QUARTERS; i++) {
        if(state->battery_plan_eigen_zon_wh_valid[i] || state->battery_plan_actief_gestuurd_wh_valid[i] ||
           state->battery_plan_verwacht_verbruik_wh_valid[i] || state->battery_soc_forecast_pct_valid[i]) {
            any_valid = true;
            break;
        }
    }

    screen_common_set_hidden(g_empty_label, any_valid);
    screen_common_set_hidden(g_line_chart, !any_valid);
    if(!any_valid) {
        /* Charts (and any tooltip on them) are hidden in this branch, but
         * clear the cache anyway so a stale tap-tooltip doesn't show old
         * data if this screen somehow becomes visible again before a
         * fresh valid update arrives. */
        memset(g_eigen_zon_valid, 0, sizeof(g_eigen_zon_valid));
        memset(g_actief_gestuurd_valid, 0, sizeof(g_actief_gestuurd_valid));
        memset(g_verwacht_verbruik_valid, 0, sizeof(g_verwacht_verbruik_valid));
        memset(g_geblokkeerd_valid, 0, sizeof(g_geblokkeerd_valid));
        memset(g_soc_valid, 0, sizeof(g_soc_valid));
        return;
    }

    int32_t values[N_QUARTERS];

    fill_series_values(values, state->battery_plan_eigen_zon_wh_valid, state->battery_plan_eigen_zon_wh,
                        WH_PER_KWARTIER_TO_W);
    lv_chart_set_series_values(g_line_chart, g_eigen_zon_series, values, N_QUARTERS);
    memcpy(g_eigen_zon_valid, state->battery_plan_eigen_zon_wh_valid, sizeof(g_eigen_zon_valid));
    cache_curve_w(g_eigen_zon_w, state->battery_plan_eigen_zon_wh);

    fill_series_values(values, state->battery_plan_actief_gestuurd_wh_valid, state->battery_plan_actief_gestuurd_wh,
                        WH_PER_KWARTIER_TO_W);
    lv_chart_set_series_values(g_line_chart, g_actief_gestuurd_series, values, N_QUARTERS);
    memcpy(g_actief_gestuurd_valid, state->battery_plan_actief_gestuurd_wh_valid, sizeof(g_actief_gestuurd_valid));
    cache_curve_w(g_actief_gestuurd_w, state->battery_plan_actief_gestuurd_wh);

    fill_series_values(values, state->battery_plan_verwacht_verbruik_wh_valid, state->battery_plan_verwacht_verbruik_wh,
                        WH_PER_KWARTIER_TO_W);
    lv_chart_set_series_values(g_line_chart, g_verwacht_verbruik_series, values, N_QUARTERS);
    memcpy(g_verwacht_verbruik_valid, state->battery_plan_verwacht_verbruik_wh_valid, sizeof(g_verwacht_verbruik_valid));
    cache_curve_w(g_verwacht_verbruik_w, state->battery_plan_verwacht_verbruik_wh);

    fill_series_values(values, state->battery_plan_geblokkeerd_wh_valid, state->battery_plan_geblokkeerd_wh,
                        WH_PER_KWARTIER_TO_W);
    lv_chart_set_series_values(g_line_chart, g_geblokkeerd_series, values, N_QUARTERS);
    memcpy(g_geblokkeerd_valid, state->battery_plan_geblokkeerd_wh_valid, sizeof(g_geblokkeerd_valid));
    cache_curve_w(g_geblokkeerd_w, state->battery_plan_geblokkeerd_wh);

    /* SOC forecast arrives as a percentage, plotted as-is (scale 1.0) on
     * its own SECONDARY_Y axis (SOC_AXIS_MIN/MAX, fixed 0-100) - no longer
     * converted to Wh/capacity like before this axis existed. */
    fill_series_values(values, state->battery_soc_forecast_pct_valid, state->battery_soc_forecast_pct, 1.0);
    lv_chart_set_series_values(g_line_chart, g_batterijlading_series, values, N_QUARTERS);
    memcpy(g_soc_valid, state->battery_soc_forecast_pct_valid, sizeof(g_soc_valid));
    memcpy(g_soc_pct, state->battery_soc_forecast_pct, sizeof(g_soc_pct));
}
