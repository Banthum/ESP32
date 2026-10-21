#include "dashboard_ui.h"

#include <lvgl.h>

#include "ipc_client.h"
#include "screen_common.h"
#include "screen_price.h"
#include "screen_consumption.h"
#include "screen_solar.h"
#include "screen_battery.h"
#include "screen_savings.h"
#include "screen_settings.h"
#include "screen_battery_settings.h"
#include "screen_price_settings.h"
#include "screen_savings_settings.h"
#include "screen_energyflow.h"
#include "fonts_signage.h"

#define UI_TIMER_PERIOD_MS 500
/* Tall enough to fully contain the hamburger button below (44px, see
 * build_header()) without clipping it - it used to be 24, sized only for
 * the title/settings row; the tabview below is pushed down by the same
 * amount since its position/size are derived from this constant. */
#define HEADER_HEIGHT UI_SY(44)

/* '\n'-separated, in the same order as the lv_tabview_add_tab() calls in
 * dashboard_ui_init() below — the hamburger menu's option index must line
 * up 1:1 with the tab index passed to lv_tabview_set_active(). Energiestromen
 * first (2026-10-01, on request: top of the hamburger list, not just the
 * default-active tab it already was). */
#define TAB_NAMES "Energiestromen\nKosten\nVerbruik\nZon\nBatterij\nBesparing"
#define TAB_INDEX_ENERGYFLOW 0 /* "Energiestromen" - must match its position in TAB_NAMES above */
#define TAB_INDEX_PRICE 1      /* "Kosten" - must match its position in TAB_NAMES above */
#define TAB_INDEX_SOLAR 3      /* "Zon" - must match its position in TAB_NAMES above */
#define TAB_INDEX_BATTERY 4    /* "Batterij" - must match its position in TAB_NAMES above, real IPC-driven plan since 2026-09-30 (was "(preview)") */
#define TAB_INDEX_SAVINGS 5    /* "Besparing" - must match its position in TAB_NAMES above, settings added 2026-10-05 (tellers-reset) */

static lv_obj_t * g_waiting_label;
static lv_obj_t * g_menu_dropdown; /* set in build_header(), read back in dashboard_ui_init() to sync default tab */
static lv_obj_t * g_tabview; /* ESP32: voor dashboard_ui_select_tab() */

/* One shared header settings icon, not one per tab (live-observed: a
 * separate copy on the Zon tab on top of this one showed two overlapping
 * gear icons) - behaviour depends on which tab is active. `tabview` comes
 * in as user_data, same pattern as menu_dropdown_event_cb() below. */
static void settings_btn_event_cb(lv_event_t * e)
{
    lv_obj_t * tabview = (lv_obj_t *) lv_event_get_user_data(e);
    uint32_t active = lv_tabview_get_tab_active(tabview);
    if(active == TAB_INDEX_PRICE) {
        screen_price_settings_open();
    }
    else if(active == TAB_INDEX_SOLAR) {
        screen_settings_open();
    }
    else if(active == TAB_INDEX_BATTERY) {
        screen_battery_settings_open();
    }
    else if(active == TAB_INDEX_SAVINGS) {
        screen_savings_settings_open();
    }
    else {
        /* Stub: general (non-tab-specific) app settings are a later
         * session. */
        LV_LOG_USER("Settings icon tapped (not implemented yet for this tab)");
    }
}

static void menu_dropdown_event_cb(lv_event_t * e)
{
    lv_obj_t * dropdown = lv_event_get_target_obj(e);
    lv_obj_t * tabview = (lv_obj_t *) lv_event_get_user_data(e);
    uint32_t selected = lv_dropdown_get_selected(dropdown);
    lv_tabview_set_active(tabview, selected, LV_ANIM_ON);
}

/* Navigation is a hamburger icon + dropdown list instead of lv_tabview's
 * own always-visible tab bar (removed via tab_bar_size=0 in
 * dashboard_ui_init()) — requested explicitly instead of visible tabs.
 * lv_dropdown with a fixed lv_dropdown_set_text_static() button label
 * doubles as an icon-only menu button: it always shows LV_SYMBOL_LIST
 * regardless of which option was last picked, rather than the normal
 * combobox behaviour of showing the current selection. */
static void build_header(lv_obj_t * parent, lv_obj_t * tabview)
{
    lv_obj_t * menu = lv_dropdown_create(parent);
    g_menu_dropdown = menu;
    lv_dropdown_set_options_static(menu, TAB_NAMES);
    lv_dropdown_set_text_static(menu, LV_SYMBOL_LIST);
    /* The dropdown draws its own arrow indicator (default LV_SYMBOL_DOWN)
     * *in addition to* the button text — live-observed: at this small
     * size the arrow visually dominated and the LV_SYMBOL_LIST text
     * wasn't recognizable as a hamburger icon at all. NULL removes that
     * separate arrow entirely, leaving just our own icon. */
    lv_dropdown_set_symbol(menu, NULL);
    /* A bit wider than the icon strictly needs (34px) — some margin
     * against known touch-precision imprecision near screen edges (see
     * CLAUDE.md's "Touchscreen X/Y-kalibratie"), since this button sits
     * right in the top-left corner. Height doubled vs. what the old,
     * shorter HEADER_HEIGHT used to allow for the same reason
     * (live-observed: only part of a HEADER_HEIGHT-4-tall button reliably
     * registered taps) — width unchanged. HEADER_HEIGHT itself was grown
     * to fully contain this taller button (see its #define). */
    lv_obj_set_size(menu, UI_S(44), UI_SY(40));
    lv_obj_set_pos(menu, UI_SX(4), UI_SY(2));
    /* Bigger than the default theme font so the icon fills the now-larger
     * button instead of looking small/lost in the middle of it. (ESP32: 30
     * i.p.v. 20, zelfde 1,5x als de rest van de UI.) */
    lv_obj_set_style_text_font(menu, &lv_font_montserrat_30, 0);
    /* Dark green header icon button on the black background - blue=0 is
     * deliberate (not just "a green"): satisfies fb_ili9486's
     * BGR-wiring-quirk safety rule (see CLAUDE.md) regardless of the
     * green value, so bgr_fix() below is a no-op here but kept for
     * consistency with every other explicit color in this app. Black icon
     * on top for contrast against the bg. */
    lv_obj_set_style_bg_color(menu, bgr_fix(lv_color_make(0, 100, 0)), 0);
    lv_obj_set_style_bg_opa(menu, LV_OPA_COVER, 0);
    /* White, not black - black-on-dark-green has poor contrast (was fine
     * on the old bright-green bg, not anymore). */
    lv_obj_set_style_text_color(menu, lv_color_white(), 0);
    lv_obj_add_event_cb(menu, menu_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, tabview);

    lv_obj_t * title = lv_label_create(parent);
    lv_label_set_text_static(title, "EMSmachine dashboard");
    lv_obj_set_pos(title, UI_SX(4) + UI_S(44) + UI_S(6), UI_SY(4));
    lv_obj_set_style_text_font(title, &lv_font_signage_18, 0);

    /* General app settings (still a stub) - separate from the Zon tab's
     * own gear button, which opens the solar-specific settings screen and
     * only makes sense there (see screen_solar.c). Same size/icon font as
     * the hamburger button above, kept in visual sync. */
    lv_obj_t * settings_btn = lv_button_create(parent);
    lv_obj_set_size(settings_btn, UI_S(44), UI_SY(40));
    lv_obj_align(settings_btn, LV_ALIGN_TOP_RIGHT, -UI_SX(6), UI_SY(2));
    lv_obj_set_style_bg_color(settings_btn, bgr_fix(lv_color_make(0, 100, 0)), 0);
    lv_obj_set_style_bg_opa(settings_btn, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(settings_btn, settings_btn_event_cb, LV_EVENT_CLICKED, tabview);
    lv_obj_t * settings_icon = lv_label_create(settings_btn);
    lv_label_set_text_static(settings_icon, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(settings_icon, &lv_font_montserrat_30, 0);
    lv_obj_set_style_text_color(settings_icon, lv_color_white(), 0); /* contrast on dark green, see menu above */
    lv_obj_center(settings_icon);

    /* Global connection status, shown across every tab rather than
     * duplicated per screen — connection state isn't screen-specific. */
    g_waiting_label = lv_label_create(parent);
    lv_label_set_text_static(g_waiting_label, "Wachten op data...");
    lv_obj_set_style_text_font(g_waiting_label, &lv_font_signage_14, 0);
    /* Pure red (255,0,0), not LV_PALETTE_RED (244,67,54) - the latter hits
     * all three conditions of the fb_ili9486 scramble bug (see CLAUDE.md
     * "Scherm" section: green 1-254 AND blue>0 AND red!=blue, checked
     * against the logical pre-bgr_fix color) and renders as purple/pink on
     * real hardware, exactly like the chart-cursor issue documented in
     * screen_price.c - just never fixed here. b=0 makes this safe. */
    lv_obj_set_style_text_color(g_waiting_label, bgr_fix(lv_color_make(255, 0, 0)), 0);
    lv_obj_center(g_waiting_label);
}

static void apply_state_timer_cb(lv_timer_t * timer)
{
    (void) timer;

    const dashboard_state_t * state = ipc_client_get_state();

    lv_obj_set_hidden(g_waiting_label, state->connected);

    screen_price_update(state);
    screen_consumption_update(state);
    screen_solar_update(state);
    screen_battery_update(state);
    screen_energyflow_update(state);
    screen_savings_update(state);
}

void dashboard_ui_init(void)
{
    lv_obj_t * screen = lv_screen_active();
    /* App-wide dark background. bg_color isn't an inheritable style
     * property in LVGL, so each tab tile below also gets it explicitly -
     * only text_color/text_font are inherited down to labels that don't
     * set their own color (chart titles, captions, the header title). */
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);
    lv_obj_set_scrollable(screen, false);

    lv_obj_t * tabview = lv_tabview_create(screen);
    g_tabview = tabview;
    lv_obj_set_size(tabview, LV_PCT(100), UI_SCREEN_H - HEADER_HEIGHT);
    lv_obj_set_pos(tabview, 0, HEADER_HEIGHT);
    lv_tabview_set_tab_bar_size(tabview, 0); /* hidden: hamburger dropdown in the header replaces it */
    lv_obj_set_style_bg_color(tabview, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tabview, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(tabview, false);
    /* The actual swipe-between-tabs behaviour lives on lv_tabview's
     * internal content container (a separate flex-row object returned by
     * lv_tabview_get_content()), not on the tabview object itself -
     * disabling scrollable on `tabview` above didn't touch it, live-
     * confirmed swiping still worked. Only the hamburger dropdown should
     * switch tabs now (see menu_dropdown_event_cb()). */
    lv_obj_set_scrollable(lv_tabview_get_content(tabview), false);

    lv_obj_t * header = lv_obj_create(screen);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), HEADER_HEIGHT);
    lv_obj_set_pos(header, 0, 0);
    build_header(header, tabview);

    lv_obj_t * tab_energyflow = lv_tabview_add_tab(tabview, "Energiestromen");
    lv_obj_t * tab_price = lv_tabview_add_tab(tabview, "Kosten");
    lv_obj_t * tab_consumption = lv_tabview_add_tab(tabview, "Verbruik");
    lv_obj_t * tab_solar = lv_tabview_add_tab(tabview, "Zon");
    lv_obj_t * tab_battery = lv_tabview_add_tab(tabview, "Batterij");
    lv_obj_t * tab_savings = lv_tabview_add_tab(tabview, "Besparing");

    /* Each tab tile is its own object with its own default (light-theme)
     * card background AND its own local default text_color - LVGL's
     * default theme runs in light mode here (CONFIG_LV_THEME_DEFAULT_DARK
     * is not set), so that per-object default text_color is a dark grey
     * meant for a light background. bg_color doesn't inherit (handled
     * below), but text_color DOES inherit - except each tab's own local
     * theme-supplied value is a closer ancestor than `screen` above, so it
     * wins over screen's white and never actually reaches labels inside
     * (live-confirmed by reading the running framebuffer: title text
     * measured at RGB(33,32,33), not the intended white - the header title
     * only looks right by accident, because build_header()'s
     * lv_obj_remove_style_all(header) also strips header's own theme
     * text_color, letting screen's white show through for that one case).
     * Force white here too so every tab actually gets what the comment
     * above always intended. Charts inside each tab keep their own white
     * card style untouched (only these tile backgrounds change, not the
     * lv_chart widgets themselves). */
    /* Not scrollable either - the fixed pixel-positioned widgets inside
     * each tab (charts, labels) are laid out to already fit the 480x320
     * panel; any drag panning was a symptom of the touch-mapping bug in
     * touch_input.c, not a feature (see CLAUDE.md), so it's turned off
     * outright now that mapping is fixed instead of relying on taps
     * happening to land where dragging wouldn't trigger. */
    lv_obj_t * tabs[] = {tab_energyflow, tab_price, tab_consumption, tab_solar, tab_battery, tab_savings};
    for(size_t i = 0; i < sizeof(tabs) / sizeof(tabs[0]); i++) {
        lv_obj_set_style_bg_color(tabs[i], lv_color_black(), 0);
        lv_obj_set_style_bg_opa(tabs[i], LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(tabs[i], lv_color_white(), 0);
        lv_obj_set_scrollable(tabs[i], false);
        /* Theme default gives each tab page ~16px of padding, which
         * silently shifted every child's position away from the literal
         * pixel coordinates screen_*.c hands to lv_obj_set_pos() (children
         * are placed relative to the parent's content area, inside its
         * padding, not its outer bounds) - contradicted the "fixed
         * pixel-positioned widgets...already fit the 480x320 panel"
         * assumption above. Pushed the price/solar charts' right edge (and
         * anything anchored to it, e.g. the last time-axis label) far
         * enough right to clip off the visible 480px-wide canvas
         * (live-confirmed via a raw /dev/fb0 pixel scan: chart background
         * ran edge-to-edge to the last column with no border visible,
         * "24:00" rendered as "24:0"). Zeroing padding here restores literal
         * pixel positioning for every tab's children. */
        lv_obj_set_style_pad_all(tabs[i], 0, 0);
    }

    screen_energyflow_build(tab_energyflow);
    screen_price_build(tab_price);
    screen_consumption_build(tab_consumption);
    screen_solar_build(tab_solar);
    screen_battery_build(tab_battery);
    screen_savings_build(tab_savings);

    screen_settings_init(screen);
    screen_battery_settings_init(screen);
    screen_price_settings_init(screen);
    screen_savings_settings_init(screen);

    /* Energiestromen is TAB_INDEX_ENERGYFLOW==0 now (2026-10-01: moved to
     * the top of TAB_NAMES, on request), so this matches lv_tabview's own
     * index-0 default already - kept explicit anyway (and in sync with
     * the dropdown's own selected index below) rather than relying on
     * that default, in case the tab order ever changes again. */
    lv_tabview_set_active(tabview, TAB_INDEX_ENERGYFLOW, LV_ANIM_OFF);
    lv_dropdown_set_selected(g_menu_dropdown, TAB_INDEX_ENERGYFLOW); /* keep the hamburger list's own selection in sync */

    lv_timer_create(apply_state_timer_cb, UI_TIMER_PERIOD_MS, NULL);
}

void dashboard_ui_select_tab(int index)
{
    if(g_tabview == NULL || index < 0 || index > TAB_INDEX_SAVINGS) return;
    lv_tabview_set_active(g_tabview, (uint32_t) index, LV_ANIM_OFF);
    lv_dropdown_set_selected(g_menu_dropdown, (uint32_t) index);
}
