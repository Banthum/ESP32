#ifndef SCREEN_COMMON_H
#define SCREEN_COMMON_H

#include <lvgl.h>

/* ESP32/CrowPanel (800x480): de schermen zijn op de Pi met vaste pixels voor
 * 480x320 ontworpen. Alle coördinaten gaan door deze macro's i.p.v. overal
 * nieuwe getallen te verzinnen, zodat de layout 1-op-1 herkenbaar blijft en
 * naast de Pi-code te leggen is:
 *   UI_SX: horizontaal (x, breedte)  480 -> 800  (x5/3)
 *   UI_SY: verticaal (y, hoogte)     320 -> 480  (x3/2)
 *   UI_S:  uniform x3/2 - voor dingen die rond/vierkant moeten blijven
 *          (cirkels, iconen) en offsets die met de lettergrootte meeschalen
 *          (de signage-fonts zijn ook 1,5x: 10/14/18 -> 15/21/27 px). */
#define UI_SX(v) ((int32_t) ((v) * 5 / 3))
#define UI_SY(v) ((int32_t) ((v) * 3 / 2))
#define UI_S(v) ((int32_t) ((v) * 3 / 2))
#define UI_SCREEN_W 800
#define UI_SCREEN_H 480

/* This panel is wired BGR, not RGB (documented ILI9486 quirk, see
 * CLAUDE.md's "Scherm" section) — LVGL's fbdev driver does not compensate
 * for it, so explicit colors come out red/blue-swapped unless corrected.
 * Every screen module must wrap its own explicit color choices with this
 * (not applied to the default theme's own internal colors — known,
 * documented residual gap, see CLAUDE.md). */
lv_color_t bgr_fix(lv_color_t c);

/* Adds "00:00"/"06:00"/"12:00"/"18:00"/"24:00" labels evenly spaced under
 * a chart at (chart_x, label_y), spanning chart_width — used by every
 * 24h/96-quarter time-series screen instead of fighting LVGL's tick-label
 * formatting API for a fixed, known axis range. Thin wrapper around
 * screen_common_add_time_axis_labels_n() below with that fixed 5-mark set. */
void screen_common_add_time_axis_labels(lv_obj_t * parent, int32_t chart_x, int32_t chart_width, int32_t label_y);

/* General form of the above - evenly spaces `n` caller-supplied `marks`
 * under a chart. Added for screen_battery.c's 48h (vandaag+morgen) axis
 * (2026-09-30) without changing the other, still-24h screens' behavior -
 * same "evenly spaced, two ends nudged inward" layout math either way. */
void screen_common_add_time_axis_labels_n(lv_obj_t * parent, int32_t chart_x, int32_t chart_width, int32_t label_y,
                                           const char * const marks[], int n);

/* Adds min/mid/max value labels to the left of a chart's Y axis, plus a
 * unit tag rotated 90° and vertically centered alongside the axis (reads
 * bottom-to-top, standard axis-title convention - requires the caller to
 * reserve ~44px of left margin for chart_x, not just ~32px, since the
 * rotated unit tag needs its own narrow column to the left of the number
 * column so it doesn't overlap the numbers). Matches lv_chart's own
 * default 3-line horizontal grid (top/middle/bottom, LV_CHART_HDIV_DEF in
 * lv_chart.c) rather than querying it, same "known fixed layout, don't
 * fight the widget's tick-label API" reasoning as the time-axis helper
 * above — this LVGL build has no lv_chart_set_axis_tick()-equivalent at
 * all. `value_divisor` converts the chart's internal integer units (e.g.
 * milli-EUR, Wh) to the displayed unit (e.g. 1000 for Wh -> kWh).
 * `out_value_labels` (nullable) receives the 3 created label objects
 * [max, mid, min], for callers whose axis range can change later (e.g.
 * screen_solar.c's customer-configured kWp) and need to update the text
 * via screen_common_set_axis_label_value() below instead of recreating
 * everything. */
void screen_common_add_y_axis_labels(lv_obj_t * parent, int32_t chart_x, int32_t chart_y, int32_t chart_height,
                                      int32_t axis_min, int32_t axis_max, int32_t value_divisor, const char * unit,
                                      lv_obj_t * out_value_labels[3]);

/* Reformats one of the 3 labels returned via out_value_labels above, for
 * an axis range that changed after screen_common_add_y_axis_labels() was
 * first called. */
void screen_common_set_axis_label_value(lv_obj_t * label, int32_t value, int32_t value_divisor);

/* Mirror of screen_common_add_y_axis_labels() for a chart's SECONDARY
 * (right-hand) Y axis - min/mid/max labels plus a rotated unit tag, both
 * placed to the right of the chart instead of the left. `chart_right_x`
 * is the chart's right edge (chart_x + chart_width), not chart_x - the
 * caller must reserve matching right-margin space, same idea as the left
 * axis's chart_x margin requirement above. `out_value_labels`/
 * `out_unit_label` (both nullable) work like screen_common_add_y_axis_
 * labels()'s out_value_labels - for callers whose axis range AND unit can
 * change later (e.g. screen_price.c's gas kWh/m3 toggle), update via
 * screen_common_set_axis_label_value()/screen_common_set_axis_unit_label()
 * below instead of recreating everything. */
void screen_common_add_y_axis_labels_right(lv_obj_t * parent, int32_t chart_right_x, int32_t chart_y,
                                            int32_t chart_height, int32_t axis_min, int32_t axis_max,
                                            int32_t value_divisor, const char * unit,
                                            lv_obj_t * out_value_labels[3], lv_obj_t ** out_unit_label);

/* Updates a rotated axis-unit label's text and re-centers it around
 * (center_x, center_y) - for the same "axis unit itself can change, not
 * just the range" callers out_unit_label above serves. Works for either
 * the left or right helper's unit label (both use the same rotation-
 * pivot trick internally). */
void screen_common_set_axis_unit_label(lv_obj_t * unit_label, const char * unit, int32_t center_x, int32_t center_y);

/* Standard "no data yet" placeholder: a gray label centered over
 * `align_to` (typically the chart it stands in for). `parent` must be an
 * ancestor of `align_to`, e.g. the same tab/screen the chart itself is a
 * child of — NOT `align_to` itself. Parenting the label to the chart
 * would make it inherit the chart's hidden state (LVGL never draws a
 * hidden object's children, live-confirmed as a real bug during this
 * screen's build — see CLAUDE.md), defeating the whole point of showing
 * this label exactly when the chart is hidden. hidden/shown independently
 * via screen_common_set_hidden(). */
lv_obj_t * screen_common_add_empty_state_label(lv_obj_t * parent, lv_obj_t * align_to, const char * text);

void screen_common_set_hidden(lv_obj_t * obj, bool hidden);

/* Fills `buf` with the tooltip text for data index `index` (0-based, same
 * indexing as the chart's own point count) - called by the tap handler
 * screen_common_add_chart_tooltip() registers below. `user_data` is
 * whatever was passed to that call, unchanged - typically a pointer to
 * the screen's own cached last-seen curve data, since a chart's data
 * isn't otherwise retained anywhere between its _update() calls. */
typedef void (*screen_common_tooltip_fill_cb_t)(void * user_data, int index, char * buf, size_t buf_size);

/* Tap-anywhere-on-the-chart value readout, added 2026-09-27 on request.
 * Adds a small label, fixed in `chart`'s top-left corner and hidden until
 * the first tap, that shows/updates `fill_cb`'s text for the data index
 * nearest the tap's x position - stays visible until the next tap (no
 * auto-hide timer, per Jeroen's preference). `n_points` must match the
 * chart's own lv_chart_set_point_count() value, used to convert an
 * absolute tap x (read via the active indev at click time, same
 * coordinate space lv_obj_get_coords() reports the chart in - already
 * rotation-correct, see touch_input.c) into a 0..n_points-1 index. Makes
 * `chart` clickable if it wasn't already. */
void screen_common_add_chart_tooltip(lv_obj_t * chart, int n_points, screen_common_tooltip_fill_cb_t fill_cb,
                                      void * user_data);

#endif /* SCREEN_COMMON_H */
