#include "screen_common.h"
#include "fonts_signage.h"

#include <stdio.h>
#include <string.h>

/* ESP32/CrowPanel: het RGB-paneel is gewoon RGB bedraad (bevestigd met de
 * kleurbalken-test in fase 1) - geen BGR-swap nodig. De functie blijft
 * bestaan zodat alle schermcode ongewijzigd kan blijven. */
lv_color_t bgr_fix(lv_color_t c)
{
    return c;
}

void screen_common_add_time_axis_labels(lv_obj_t * parent, int32_t chart_x, int32_t chart_width, int32_t label_y)
{
    static const char * const marks[] = {"00:00", "06:00", "12:00", "18:00", "24:00"};
    screen_common_add_time_axis_labels_n(parent, chart_x, chart_width, label_y, marks, 5);
}

void screen_common_add_time_axis_labels_n(lv_obj_t * parent, int32_t chart_x, int32_t chart_width, int32_t label_y,
                                           const char * const marks[], int n)
{
    for(int i = 0; i < n; i++) {
        lv_obj_t * label = lv_label_create(parent);
        lv_label_set_text_static(label, marks[i]);
        lv_obj_set_style_text_font(label, &lv_font_signage_14, 0);
        /* Evenly spaced across the chart width; the two ends are nudged
         * inward so the label text doesn't overhang the chart edges
         * (labels aren't centered on a point by lv_obj_set_pos). The last
         * label is pulled back by its own measured width rather than a
         * fixed pixel guess - a fixed guess tuned for one font's glyph
         * widths clips under a wider one (live-confirmed: "24:00" fell
         * off-screen after switching from the old condensed DIN font to
         * the wider Oswald). lv_obj_update_layout() is needed first to
         * resolve the label's natural (shrink-wrapped) width. */
        int32_t x = chart_x + (chart_width * i) / (n - 1);
        if(i == 0) x += UI_S(2);
        if(i == n - 1) {
            lv_obj_update_layout(label);
            x = chart_x + chart_width - lv_obj_get_width(label);
        }
        lv_obj_set_pos(label, x, label_y);
    }
}

/* Formats value/divisor with at most 1 decimal, dropping a trailing ".0"
 * (e.g. 5000/1000 -> "5", 2500/1000 -> "2.5") - chart axis ranges in this
 * app are round numbers, so this never needs more precision than that. */
static void format_axis_value(char * out, size_t out_size, int32_t value, int32_t divisor)
{
    double scaled = (double) value / (double) divisor;
    snprintf(out, out_size, "%.1f", scaled);
    size_t len = strlen(out);
    if(len >= 2 && out[len - 1] == '0' && out[len - 2] == '.') {
        out[len - 2] = '\0';
    }
}

void screen_common_add_y_axis_labels(lv_obj_t * parent, int32_t chart_x, int32_t chart_y, int32_t chart_height,
                                      int32_t axis_min, int32_t axis_max, int32_t value_divisor, const char * unit,
                                      lv_obj_t * out_value_labels[3])
{
    /* Top (max), middle, bottom (min) - matches lv_chart's default 3-line
     * horizontal grid, see header comment. */
    const int32_t values[3] = {axis_max, (axis_min + axis_max) / 2, axis_min};
    for(int i = 0; i < 3; i++) {
        lv_obj_t * label = lv_label_create(parent);
        screen_common_set_axis_label_value(label, values[i], value_divisor);
        lv_obj_set_style_text_font(label, &lv_font_signage_18, 0);
        /* Fixed-width + right-aligned text so "5" and "2.5" both end at
         * the same x, flush against the chart's left edge. Vertically
         * nudged up by half of lv_font_signage_18's line_height (20/2=10)
         * so each label straddles its grid line (top/middle/bottom)
         * instead of sitting entirely below it - matches the title's font
         * size now (bumped up from 14px on Jeroen's request, previously
         * felt harder to read than the title). Box widened from 22 to 26
         * to fit the wider 18px glyphs ("0.5" measures ~23px at this
         * size) without wrapping/clipping. */
        lv_obj_set_pos(label, chart_x - UI_S(30), chart_y + (chart_height * i) / 2 - UI_S(10));
        lv_obj_set_width(label, UI_S(26));
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
        if(out_value_labels != NULL) out_value_labels[i] = label;
    }

    /* Rotated 90° (reads bottom-to-top, standard vertical-axis-title
     * convention) and centered on the axis's vertical midpoint, in its
     * own narrow column further left than the number column above (see
     * header comment on the extra left-margin this needs). Rotation
     * pivots around the label's own center (50%/50%), so positioning it
     * is just "put the label's un-rotated center where the rotated
     * glyph's visual center should end up" - lv_obj_update_layout() is
     * needed first to resolve the label's natural (un-rotated) size,
     * since it's created without an explicit lv_obj_set_size(). */
    lv_obj_t * unit_label = lv_label_create(parent);
    lv_label_set_text(unit_label, unit);
    lv_obj_set_style_text_font(unit_label, &lv_font_signage_18, 0);
    /* White, not grey - was grey (matching other secondary captions
     * elsewhere in the UI) but on Jeroen's request this now matches the
     * title's full-white weight, since rotated text is already harder to
     * read than horizontal and grey made it worse. */
    lv_obj_set_style_text_color(unit_label, lv_color_white(), 0);
    lv_obj_update_layout(unit_label);
    int32_t natural_w = lv_obj_get_width(unit_label);
    int32_t natural_h = lv_obj_get_height(unit_label);
    /* Shifted further left than before (-36 -> -42) - the bigger 18px
     * unit text is wider once rotated (~natural_h, now ~20px vs ~16px
     * before), and would otherwise overlap the wider number column above. */
    int32_t center_x = chart_x - UI_S(42);
    int32_t center_y = chart_y + chart_height / 2;
    lv_obj_set_pos(unit_label, center_x - natural_w / 2, center_y - natural_h / 2);
    lv_obj_set_style_transform_pivot_x(unit_label, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(unit_label, lv_pct(50), 0);
    lv_obj_set_style_transform_rotation(unit_label, 2700, 0); /* -90 degrees, in 0.1deg units */
}

void screen_common_set_axis_label_value(lv_obj_t * label, int32_t value, int32_t value_divisor)
{
    char buf[16];
    format_axis_value(buf, sizeof(buf), value, value_divisor);
    lv_label_set_text(label, buf);
}

/* Re-centers a rotated axis-unit label (left or right helper, both use the
 * same pivot-at-50%/50% rotation trick) around (center_x, center_y) after
 * its text changed - e.g. screen_price.c switching gas units between
 * "EUR/kWh" and "EUR/m3", which have different natural (un-rotated)
 * widths. Must re-measure via lv_obj_update_layout() same as at creation
 * time, since the new text's natural size differs from the old. */
void screen_common_set_axis_unit_label(lv_obj_t * unit_label, const char * unit, int32_t center_x, int32_t center_y)
{
    lv_label_set_text(unit_label, unit);
    lv_obj_update_layout(unit_label);
    int32_t natural_w = lv_obj_get_width(unit_label);
    int32_t natural_h = lv_obj_get_height(unit_label);
    lv_obj_set_pos(unit_label, center_x - natural_w / 2, center_y - natural_h / 2);
}

void screen_common_add_y_axis_labels_right(lv_obj_t * parent, int32_t chart_right_x, int32_t chart_y,
                                            int32_t chart_height, int32_t axis_min, int32_t axis_max,
                                            int32_t value_divisor, const char * unit,
                                            lv_obj_t * out_value_labels[3], lv_obj_t ** out_unit_label)
{
    /* Mirror of screen_common_add_y_axis_labels() above - see its comments
     * for why each offset/nudge is what it is (same font, same straddle
     * logic, just left-aligned and positioned right of the chart instead
     * of right-aligned/left of it). */
    const int32_t values[3] = {axis_max, (axis_min + axis_max) / 2, axis_min};
    for(int i = 0; i < 3; i++) {
        lv_obj_t * label = lv_label_create(parent);
        screen_common_set_axis_label_value(label, values[i], value_divisor);
        lv_obj_set_style_text_font(label, &lv_font_signage_18, 0);
        lv_obj_set_pos(label, chart_right_x + UI_S(6), chart_y + (chart_height * i) / 2 - UI_S(10));
        lv_obj_set_width(label, UI_S(34)); /* fits "100" */
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
        if(out_value_labels != NULL) out_value_labels[i] = label;
    }

    lv_obj_t * unit_label = lv_label_create(parent);
    lv_obj_set_style_text_font(unit_label, &lv_font_signage_18, 0);
    lv_obj_set_style_text_color(unit_label, lv_color_white(), 0);
    lv_obj_set_style_transform_pivot_x(unit_label, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(unit_label, lv_pct(50), 0);
    lv_obj_set_style_transform_rotation(unit_label, 2700, 0);
    screen_common_set_axis_unit_label(unit_label, unit, chart_right_x + UI_S(46), chart_y + chart_height / 2);
    if(out_unit_label != NULL) *out_unit_label = unit_label;
}

lv_obj_t * screen_common_add_empty_state_label(lv_obj_t * parent, lv_obj_t * align_to, const char * text)
{
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_signage_18, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_align_to(label, align_to, LV_ALIGN_CENTER, 0, 0);
    return label;
}

void screen_common_set_hidden(lv_obj_t * obj, bool hidden)
{
    lv_obj_set_hidden(obj, hidden);
}

typedef struct {
    lv_obj_t * tooltip_label;
    int n_points;
    screen_common_tooltip_fill_cb_t fill_cb;
    void * user_data;
} chart_tooltip_ctx_t;

static void chart_tooltip_event_cb(lv_event_t * e)
{
    chart_tooltip_ctx_t * ctx = (chart_tooltip_ctx_t *) lv_event_get_user_data(e);
    lv_obj_t * chart = lv_event_get_target_obj(e);

    lv_indev_t * indev = lv_indev_get_act();
    if(indev == NULL) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);

    /* Absolute (screen-space) coords, same space `point` is in - not
     * chart_x/chart_y relative-to-parent values, which is why this
     * queries the object directly instead of taking them as params. */
    lv_area_t area;
    lv_obj_get_coords(chart, &area);
    int32_t width = area.x2 - area.x1;
    if(width <= 0) return;

    int32_t rel_x = point.x - area.x1;
    if(rel_x < 0) rel_x = 0;
    if(rel_x > width) rel_x = width;

    /* Rounded (not truncated) nearest-index: +width/2 before the divide. */
    int index = (int) ((rel_x * (ctx->n_points - 1) + width / 2) / width);
    if(index < 0) index = 0;
    if(index >= ctx->n_points) index = ctx->n_points - 1;

    char buf[128];
    ctx->fill_cb(ctx->user_data, index, buf, sizeof(buf));
    lv_label_set_text(ctx->tooltip_label, buf);
    lv_obj_set_hidden(ctx->tooltip_label, false);
    lv_obj_move_foreground(ctx->tooltip_label);
}

static void chart_tooltip_dismiss_cb(lv_event_t * e)
{
    /* Tapping the tooltip itself hides it - LVGL doesn't bubble a child's
     * click event up to its parent by default (LV_OBJ_FLAG_EVENT_BUBBLE
     * is opt-in, never set here), so this fires instead of - not in
     * addition to - chart_tooltip_event_cb() above re-showing it at the
     * same spot. */
    lv_obj_t * tooltip = lv_event_get_target_obj(e);
    lv_obj_set_hidden(tooltip, true);
}

void screen_common_add_chart_tooltip(lv_obj_t * chart, int n_points, screen_common_tooltip_fill_cb_t fill_cb,
                                      void * user_data)
{
    lv_obj_set_clickable(chart, true);

    /* Child of `chart` itself (not a tab-level sibling) so it always
     * renders on top of that chart's own series without needing its own
     * screen-specific layout math - fixed at a corner, not following the
     * tap position, so it can never end up positioned off-screen. 18px,
     * not 14 - live feedback (2026-09-27) called the smaller size too
     * hard to read here. */
    lv_obj_t * tooltip = lv_label_create(chart);
    lv_obj_set_style_text_font(tooltip, &lv_font_signage_18, 0);
    lv_obj_set_style_text_color(tooltip, lv_color_white(), 0);
    lv_obj_set_style_bg_color(tooltip, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tooltip, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(tooltip, UI_S(3), 0);
    lv_obj_set_pos(tooltip, UI_S(4), UI_S(4));
    lv_obj_set_hidden(tooltip, true);
    lv_obj_set_clickable(tooltip, true);
    lv_obj_add_event_cb(tooltip, chart_tooltip_dismiss_cb, LV_EVENT_CLICKED, NULL);

    /* Heap-allocated, never freed - matches this app's general style of
     * one-time setup that lives for the process lifetime (LVGL objects
     * themselves are never destroyed here either, see e.g. screen_solar.c's
     * out_value_labels pattern). */
    chart_tooltip_ctx_t * ctx = lv_malloc(sizeof(chart_tooltip_ctx_t));
    ctx->tooltip_label = tooltip;
    ctx->n_points = n_points;
    ctx->fill_cb = fill_cb;
    ctx->user_data = user_data;

    lv_obj_add_event_cb(chart, chart_tooltip_event_cb, LV_EVENT_CLICKED, ctx);
}
