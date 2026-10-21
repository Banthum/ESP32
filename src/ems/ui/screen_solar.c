#include "screen_solar.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "screen_common.h"
#include "fonts_signage.h"

/* 2-daags (vandaag+morgen), kwartier-resolutie voor de zonlijn - zelfde
 * 192-puntsconventie als screen_battery.c (2026-10-04, Jeroen: "ik wil
 * dat het zonverwachting dashboard uitgebreid wordt naar twee dagen"). */
#define N_QUARTERS IPC_BATTERY_PLAN_LEN
/* Verwacht-verbruik-staven op UUR-resolutie, niet kwartier (Jeroen's
 * expliciete keuze, 2026-10-04) - 192 staven over ~414px (<2px/staaf) is
 * een dichtgesmeerde band, geen leesbare staven (al eerder ontdekt+
 * gefixt voor Scherm D, zie CLAUDE.md - dié grafiek ging toen juist van
 * staven NAAR lijnen). 48 staven (2x 24u) blijft ruim leesbaar. */
#define N_HOURS 48

#define AXIS_MIN_W 0        /* W (kW * 1000) */
#define AXIS_MAX_W_DEFAULT 5000 /* fallback until the first real forecast data arrives */
/* Rounding granularity for the data-driven rescale below - same "0.5" step
 * Jeroen already uses for kWp elsewhere (axis_max_wh_for_kwp()'s old
 * kWp-based version), now applied to actual forecast kW instead of
 * installed capacity. */
#define AXIS_ROUND_STEP_KW 0.5

/* ESP32 (800x480): zie screen_price.c - marges x1,5, grafiek vult de breedte. */
#define CHART_X UI_S(56)
#define CHART_Y UI_SY(34)
#define CHART_WIDTH (UI_SCREEN_W - CHART_X - UI_S(10))
#define CHART_HEIGHT UI_SY(180)

static lv_obj_t * g_chart;                  /* LINE, solar, N_QUARTERS points */
static lv_chart_series_t * g_series;
static lv_chart_cursor_t * g_now_cursor;
static lv_chart_cursor_t * g_day_boundary_cursor; /* fixed at kwartier 96 (middernacht) */
static lv_obj_t * g_consumption_chart;      /* BAR, verwacht verbruik, N_HOURS points - overlaid on g_chart */
static lv_chart_series_t * g_consumption_series;
static lv_obj_t * g_empty_label;
static lv_obj_t * g_axis_labels[3]; /* [max, mid, min], see screen_common_add_y_axis_labels() */
/* NAN (not 0 or -1) so the very first real computed range - including a
 * legitimate 0 - is never mistaken for "already applied, skip" (same
 * pattern as screen_battery.c's g_axis_applied_min/max_wh). */
static double g_axis_applied_max_w = NAN;

/* Cached from the last screen_solar_update() call, for the tap-tooltip
 * (added 2026-09-27) - see screen_price.c's identical-purpose cache for
 * why this can't just read from `state` at tap time. `state`'s curve is
 * itself already kW (genuine power, since 2026-10-03 - see
 * solar_forecast_client.fetch_forecast()'s docstring), no conversion
 * needed here, just a direct copy. */
static bool g_solar_curve_valid[N_QUARTERS];
static double g_solar_curve_kw[N_QUARTERS];
/* Hourly-aggregated (mean of the 4 kwartieren in that hour, skipping any
 * individually-invalid ones) verwacht-verbruik cache, kW - computed once
 * per screen_solar_update() call from the 192-point kwartier source, not
 * per-tooltip-call. A tapped kwartier (0-191) maps to hour index/4. */
static bool g_consumption_hourly_valid[N_HOURS];
static double g_consumption_hourly_kw[N_HOURS];

static void solar_tooltip_fill(void * user_data, int index, char * buf, size_t buf_size)
{
    (void) user_data;
    int quarter_of_day = index % IPC_QUARTERS_PER_DAY;
    int hour = (quarter_of_day * 15) / 60;
    int minute = (quarter_of_day * 15) % 60;
    const char * day_prefix = index >= IPC_QUARTERS_PER_DAY ? "Morgen " : "";

    char zon_buf[16];
    if(!g_solar_curve_valid[index]) snprintf(zon_buf, sizeof(zon_buf), "-");
    else snprintf(zon_buf, sizeof(zon_buf), "%.2f kW", g_solar_curve_kw[index]);

    int hour_idx = index / 4; /* 0-47, same mapping as the N_HOURS bars */
    char verbruik_buf[16];
    if(hour_idx >= N_HOURS || !g_consumption_hourly_valid[hour_idx]) snprintf(verbruik_buf, sizeof(verbruik_buf), "-");
    else snprintf(verbruik_buf, sizeof(verbruik_buf), "%.2f kW", g_consumption_hourly_kw[hour_idx]);

    snprintf(buf, buf_size, "%s%02d:%02d\nZon: %s\nVerwacht verbruik: %s", day_prefix, hour, minute, zon_buf,
             verbruik_buf);
}

/* Rescales to whatever BOTH curves actually contain (2026-10-03, Jeroen:
 * "ik wil dat de y as meeschaalt"; extended 2026-10-04 to also cover the
 * new verwacht-verbruik bars, since they now share this one axis) rather
 * than a fixed ceiling - mirrors screen_battery.c's
 * compute_power_axis_range_wh(). Returns false (no valid data in either
 * curve) if there's nothing to scale to - caller keeps whatever range was
 * last applied in that case. */
static bool compute_combined_axis_range_w(const dashboard_state_t * state, int32_t * out_max_w)
{
    double hi = 0.0;
    bool any = false;
    for(int i = 0; i < N_QUARTERS; i++) {
        if(!state->solar_forecast_curve_quarterly_valid[i]) continue;
        double kw = state->solar_forecast_curve_quarterly[i]; /* already kW, see module docstring */
        if(kw > hi) hi = kw;
        any = true;
    }
    for(int i = 0; i < N_QUARTERS; i++) {
        if(!state->expected_consumption_curve_quarterly_valid[i]) continue;
        /* kWh PER KWARTIER (energy, not power - see
         * docs/ipc_protocol.md's expected_consumption_curve entry, same
         * source data this field is built from) -> average kW: energy /
         * 0.25h = energy * 4. Live-caught bug (2026-10-04): this was
         * wrongly treated as Watts (/1000.0) here, collapsing real
         * 0.05-0.7 kWh/kwartier values to near-zero kW - looked like
         * "all bars the same (tiny) size" on the touchscreen and
         * effectively invisible on dashboard-web. */
        double kw = state->expected_consumption_curve_quarterly[i] * 4.0;
        if(kw > hi) hi = kw;
        any = true;
    }
    if(!any) return false;

    double rounded_kw = ceil(hi / AXIS_ROUND_STEP_KW) * AXIS_ROUND_STEP_KW;
    if(rounded_kw <= 0.0) rounded_kw = AXIS_ROUND_STEP_KW; /* never a degenerate 0..0 axis */
    *out_max_w = (int32_t) lround(rounded_kw * 1000.0);
    return true;
}

void screen_solar_build(lv_obj_t * parent)
{
    lv_obj_t * title = lv_label_create(parent);
    lv_label_set_text_static(title, "Zonverwachting + verbruik (kW)");
    lv_obj_set_pos(title, UI_SX(10), UI_SY(4));
    lv_obj_set_style_text_font(title, &lv_font_signage_18, 0);

    /* Bar chart created FIRST so the solar line (created after, below)
     * draws on top of it and stays visible over/through the bars - LVGL
     * draws children in creation order. Transparent background so it
     * doesn't itself obscure the screen underneath; a muted grey bar
     * color (same "verwacht verbruik" grey screen_battery.c already uses)
     * so bars read as a secondary/background series, not competing with
     * the orange solar line for attention. */
    g_consumption_chart = lv_chart_create(parent);
    lv_obj_set_pos(g_consumption_chart, CHART_X, CHART_Y);
    lv_obj_set_size(g_consumption_chart, CHART_WIDTH, CHART_HEIGHT);
    lv_chart_set_type(g_consumption_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(g_consumption_chart, N_HOURS);
    lv_chart_set_axis_range(g_consumption_chart, LV_CHART_AXIS_PRIMARY_Y, AXIS_MIN_W, AXIS_MAX_W_DEFAULT);
    /* Gap between bar columns - LVGL's own bar-chart example
     * (lv_example_chart_recolor_bars.c) sets this explicitly rather than
     * relying on the theme default; without it the default pad_column
     * this theme applies left the bars looking washed-out/near-invisible
     * (live-caught 2026-10-04). */
    lv_obj_set_style_pad_column(g_consumption_chart, UI_S(2), 0);
    lv_obj_set_style_bg_opa(g_consumption_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(g_consumption_chart, LV_OPA_TRANSP, 0);
    g_consumption_series =
        lv_chart_add_series(g_consumption_chart, bgr_fix(lv_color_make(118, 125, 118)), LV_CHART_AXIS_PRIMARY_Y);
    /* LV_PART_ITEMS (the bars themselves) explicitly forced fully opaque -
     * only LV_PART_MAIN (the chart's own card background, selector 0
     * above) was meant to go transparent, so the solar line chart
     * underneath/behind this one in z-order doesn't get hidden by an
     * opaque card background. Set explicitly rather than assumed, in case
     * opacity ever turns out to cascade from MAIN to ITEMS in this LVGL
     * build. */
    lv_obj_set_style_bg_opa(g_consumption_chart, LV_OPA_COVER, LV_PART_ITEMS);

    /* x=56/width=414 (not the usual 10/460) - leaves room on the left for
     * screen_common_add_y_axis_labels()'s rotated kW unit tag + number
     * column (widened from 44/426 when the Y-axis labels grew from 14px
     * to 18px). */
    g_chart = lv_chart_create(parent);
    lv_obj_set_pos(g_chart, CHART_X, CHART_Y);
    lv_obj_set_size(g_chart, CHART_WIDTH, CHART_HEIGHT);
    lv_chart_set_type(g_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(g_chart, N_QUARTERS);
    lv_chart_set_axis_range(g_chart, LV_CHART_AXIS_PRIMARY_Y, AXIS_MIN_W, AXIS_MAX_W_DEFAULT);
    /* Transparent too, otherwise its own opaque card background would
     * cover the bar chart sitting directly underneath it. */
    lv_obj_set_style_bg_opa(g_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(g_chart, LV_OPA_TRANSP, 0);

    g_series = lv_chart_add_series(g_chart, bgr_fix(lv_palette_main(LV_PALETTE_ORANGE)), LV_CHART_AXIS_PRIMARY_Y);
    /* Blue + dashed "now" marker - see screen_price.c's build function for
     * why (LV_PALETTE_RED rendered purple/pink on this BGR-wired panel). */
    g_now_cursor = lv_chart_add_cursor(g_chart, bgr_fix(lv_color_make(0, 0, 255)), LV_DIR_VER);
    lv_obj_set_style_line_dash_width(g_chart, UI_S(6), LV_PART_CURSOR);
    lv_obj_set_style_line_dash_gap(g_chart, UI_S(4), LV_PART_CURSOR);
    /* Fixed vertical marker at kwartier 96 (middernacht, start of
     * "morgen") - the "Vandaag"/"Morgen" text labels below the axis
     * weren't clear enough on their own (Jeroen, 2026-10-04: "de overgang
     * naar de volgende dag is niet goed te zien"). A plain grey/white
     * line (LV_PART_CURSOR styling - dash width/gap above - is shared
     * across every cursor on this chart, so this one dashes the same way
     * the "now" marker does; distinguished by color instead). Position is
     * fixed (quarter 96 never changes), set once here rather than every
     * screen_solar_update() tick. */
    g_day_boundary_cursor = lv_chart_add_cursor(g_chart, lv_color_white(), LV_DIR_VER);
    lv_chart_set_cursor_point(g_chart, g_day_boundary_cursor, NULL, IPC_QUARTERS_PER_DAY);

    /* 48h span (vandaag+morgen) - two rows below the chart: a real
     * 00:00-24:00 time axis per day, plus the "Vandaag"/"Morgen" day
     * header underneath that - exact same layout screen_battery.c already
     * uses for its own 192-kwartier 2-day chart. */
    static const char * const day_axis_marks[] = {"00:00", "12:00", "24:00"};
    screen_common_add_time_axis_labels_n(parent, CHART_X, CHART_WIDTH / 2, CHART_Y + CHART_HEIGHT + UI_S(2), day_axis_marks,
                                          3);
    screen_common_add_time_axis_labels_n(parent, CHART_X + CHART_WIDTH / 2, CHART_WIDTH / 2,
                                          CHART_Y + CHART_HEIGHT + UI_S(2), day_axis_marks, 3);

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

    /* Placeholder range/labels until screen_solar_update() sees real
     * forecast data and rescales both via compute_combined_axis_range_w()
     * below. */
    screen_common_add_y_axis_labels(parent, CHART_X, CHART_Y, CHART_HEIGHT, AXIS_MIN_W, AXIS_MAX_W_DEFAULT, 1000,
                                     "kW", g_axis_labels);
    screen_common_add_chart_tooltip(g_chart, N_QUARTERS, solar_tooltip_fill, NULL);

    lv_obj_t * legend = lv_label_create(parent);
    lv_label_set_recolor(legend, true);
    /* #ff9800 is LV_PALETTE_ORANGE's own hex, pre-swapped the same way
     * screen_battery.c's legend already documents (BGR-wiring quirk, see
     * CLAUDE.md) -> 00c1ff wouldn't be right here since this is the *raw*
     * palette orange, not one of the custom scramble-safe colors
     * elsewhere; checked against the scramble rule (green=152, not 0/255,
     * blue=0) - blue=0 is itself always safe regardless of red/green, see
     * CLAUDE.md's "vuistregel" - so orange needed no swap at all and the
     * BGR-wired byte-order coincidentally reads the same hex either way
     * for this specific color. Grey matches g_consumption_series's actual
     * bgr_fix()-ed color (118,125,118 -> swapped 767d76). */
    lv_label_set_text_static(legend, "#ff9800 zonverwachting# / #767d76 verwacht verbruik#");
    lv_obj_set_style_text_font(legend, &lv_font_signage_14, 0);
    lv_obj_set_pos(legend, UI_SX(10), CHART_Y + CHART_HEIGHT + UI_S(36));

    /* Currently the permanent state: solar_forecast isn't populated by
     * anything yet (no Forecast.Solar integration) — see CLAUDE.md. This
     * screen is built and ready, just has nothing to show until that
     * data source exists. */
    g_empty_label = screen_common_add_empty_state_label(
        parent, g_chart, "Nog geen zonvoorspelling/verbruiksdata beschikbaar");
    lv_obj_set_style_text_align(g_empty_label, LV_TEXT_ALIGN_CENTER, 0);
}

void screen_solar_update(const dashboard_state_t * state)
{
    /* Rescale to whatever the forecast/consumption curves actually
     * contain, only re-applying when the computed range actually changed
     * (same pattern as screen_battery.c/screen_price.c's axis rescale). */
    int32_t axis_max_w;
    if(compute_combined_axis_range_w(state, &axis_max_w) && axis_max_w != g_axis_applied_max_w) {
        lv_chart_set_axis_range(g_chart, LV_CHART_AXIS_PRIMARY_Y, AXIS_MIN_W, axis_max_w);
        lv_chart_set_axis_range(g_consumption_chart, LV_CHART_AXIS_PRIMARY_Y, AXIS_MIN_W, axis_max_w);
        screen_common_set_axis_label_value(g_axis_labels[0], axis_max_w, 1000);
        screen_common_set_axis_label_value(g_axis_labels[1], axis_max_w / 2, 1000);
        screen_common_set_axis_label_value(g_axis_labels[2], AXIS_MIN_W, 1000);
        g_axis_applied_max_w = axis_max_w;
    }

    bool any_valid = false;
    for(int i = 0; i < N_QUARTERS; i++) {
        if(state->solar_forecast_curve_quarterly_valid[i] || state->expected_consumption_curve_quarterly_valid[i]) {
            any_valid = true;
            break;
        }
    }

    screen_common_set_hidden(g_empty_label, any_valid);
    screen_common_set_hidden(g_chart, !any_valid);
    screen_common_set_hidden(g_consumption_chart, !any_valid);
    if(!any_valid) {
        /* Charts (and any tooltip on them) are hidden in this branch, but
         * clear the caches anyway so a stale tap-tooltip doesn't show old
         * data if this screen somehow becomes visible again before a
         * fresh valid update arrives. */
        memset(g_solar_curve_valid, 0, sizeof(g_solar_curve_valid));
        memset(g_consumption_hourly_valid, 0, sizeof(g_consumption_hourly_valid));
        return;
    }

    int32_t values[N_QUARTERS];
    for(int i = 0; i < N_QUARTERS; i++) {
        /* Already kW - *1000.0 only for the usual internal W-scale/
         * divisor=1000 display convention (see
         * screen_common_add_y_axis_labels()), no unit conversion. */
        values[i] = state->solar_forecast_curve_quarterly_valid[i]
                        ? (int32_t) round(state->solar_forecast_curve_quarterly[i] * 1000.0)
                        : LV_CHART_POINT_NONE;
        g_solar_curve_valid[i] = state->solar_forecast_curve_quarterly_valid[i];
        g_solar_curve_kw[i] = state->solar_forecast_curve_quarterly[i];
    }
    lv_chart_set_series_values(g_chart, g_series, values, N_QUARTERS);

    /* Aggregate the 192 kwartieren down to 48 hourly bars - mean of
     * whatever subset of the 4 kwartieren in that hour are valid (never
     * fabricated if all 4 are missing, same "best available data"
     * convention as the rest of this app). Source values are kWh PER
     * KWARTIER (energy), not Watts - see compute_combined_axis_range_w()'s
     * comment above for the live-caught bug this converts correctly now
     * (energy * 4 = average kW, then *1000 for the usual internal
     * W-scale/divisor=1000 display convention). */
    int32_t hourly_values[N_HOURS];
    for(int h = 0; h < N_HOURS; h++) {
        double sum_kwh = 0.0;
        int count = 0;
        for(int k = 0; k < 4; k++) {
            int idx = h * 4 + k;
            if(!state->expected_consumption_curve_quarterly_valid[idx]) continue;
            sum_kwh += state->expected_consumption_curve_quarterly[idx];
            count++;
        }
        if(count == 0) {
            hourly_values[h] = LV_CHART_POINT_NONE;
            g_consumption_hourly_valid[h] = false;
            continue;
        }
        double mean_kw = (sum_kwh / count) * 4.0;
        hourly_values[h] = (int32_t) round(mean_kw * 1000.0);
        g_consumption_hourly_valid[h] = true;
        g_consumption_hourly_kw[h] = mean_kw;
    }
    lv_chart_set_series_values(g_consumption_chart, g_consumption_series, hourly_values, N_HOURS);

    time_t now = time(NULL);
    struct tm local_tm;
    localtime_r(&now, &local_tm);
    int quarter_of_day = (local_tm.tm_hour * 60 + local_tm.tm_min) / 15;
    lv_chart_set_cursor_point(g_chart, g_now_cursor, g_series, (uint32_t) quarter_of_day);
}
