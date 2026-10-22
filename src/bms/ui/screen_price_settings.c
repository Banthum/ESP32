#include "screen_price_settings.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "screen_common.h"
#include "fonts_signage.h"
#include "../core/paths.h"
#include "../daemon/daemon.h"

/* ESP32: LittleFS i.p.v. /etc/emsmachine/ (zie core/paths.h). */
#define GAS_CONFIG_PATH BMS_GAS_CONFIG_PATH
/* Index 0/1 must match dashboard/gas_config.py's "kwh"/"m3" unit keys -
 * same "small, rarely-changed mapping kept in sync by hand across the
 * Python/C boundary" style as screen_settings.c's RICHTING_KEYS. */
static const char * const UNIT_KEYS[] = {"kwh", "m3"};
#define UNIT_OPTIONS "EUR/kWh\nEUR/m3"

static lv_obj_t * g_settings_screen;
static lv_obj_t * g_dashboard_screen;

static lv_obj_t * g_enabled_switch;
static lv_obj_t * g_unit_dropdown;
static lv_obj_t * g_status_label;

static void back_btn_event_cb(lv_event_t * e)
{
    (void) e;
    lv_screen_load(g_dashboard_screen);
}

/* Loads the current gas_config.json (if any) into the form - one-shot
 * read when the screen opens, same reasoning as
 * screen_battery_settings.c::load_current_config(). Defaults (enabled,
 * kWh) match dashboard/gas_config.py's DEFAULT_ENABLED/DEFAULT_UNIT, for
 * a config file that doesn't exist yet. */
static void load_current_config(void)
{
    lv_label_set_text(g_status_label, "");
    lv_obj_add_state(g_enabled_switch, LV_STATE_CHECKED);
    lv_dropdown_set_selected(g_unit_dropdown, 0);

    FILE * f = fopen(GAS_CONFIG_PATH, "r");
    if(f == NULL) {
        return;
    }
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    cJSON * root = cJSON_Parse(buf);
    if(root == NULL) {
        return;
    }

    cJSON * enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    if(cJSON_IsBool(enabled)) {
        if(cJSON_IsTrue(enabled)) lv_obj_add_state(g_enabled_switch, LV_STATE_CHECKED);
        else lv_obj_remove_state(g_enabled_switch, LV_STATE_CHECKED);
    }

    cJSON * unit = cJSON_GetObjectItemCaseSensitive(root, "unit");
    if(cJSON_IsString(unit)) {
        for(size_t i = 0; i < sizeof(UNIT_KEYS) / sizeof(UNIT_KEYS[0]); i++) {
            if(strcmp(unit->valuestring, UNIT_KEYS[i]) == 0) {
                lv_dropdown_set_selected(g_unit_dropdown, (uint32_t) i);
                break;
            }
        }
    }

    cJSON_Delete(root);
}

/* Writes GAS_CONFIG_PATH directly via cJSON - no external API call needed
 * to persist this (no geocoding-equivalent step), same reasoning as
 * screen_battery_settings.c::write_config(). */
static bool write_config(bool enabled, const char * unit, char * error_out, size_t error_out_size)
{
    cJSON * root = cJSON_CreateObject();
    if(root == NULL) {
        snprintf(error_out, error_out_size, "interne fout (cJSON)");
        return false;
    }
    cJSON_AddBoolToObject(root, "enabled", enabled);
    cJSON_AddStringToObject(root, "unit", unit);

    char * json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if(json_str == NULL) {
        snprintf(error_out, error_out_size, "interne fout (cJSON-print)");
        return false;
    }

    FILE * f = fopen(GAS_CONFIG_PATH, "w");
    bool ok = false;
    if(f == NULL) {
        snprintf(error_out, error_out_size, "kon %s niet wegschrijven", GAS_CONFIG_PATH);
    }
    else {
        ok = fputs(json_str, f) >= 0;
        fclose(f);
        if(!ok) snprintf(error_out, error_out_size, "kon %s niet wegschrijven", GAS_CONFIG_PATH);
    }
    cJSON_free(json_str);
    return ok;
}

/* Pi: `systemctl restart dashboard-daemon.service`. ESP32: de daemon leest de
 * instellingen bij de volgende cyclus opnieuw in. */
static bool restart_daemon(void)
{
    daemon_reload_config();
    return true;
}

static void save_btn_event_cb(lv_event_t * e)
{
    (void) e;

    bool enabled = lv_obj_has_state(g_enabled_switch, LV_STATE_CHECKED);
    uint32_t unit_idx = lv_dropdown_get_selected(g_unit_dropdown);

    lv_label_set_text_static(g_status_label, "Bezig met opslaan...");
    lv_refr_now(NULL); /* show the "Bezig..." text before the blocking spawn below */

    char error_buf[128];
    if(!write_config(enabled, UNIT_KEYS[unit_idx], error_buf, sizeof(error_buf))) {
        lv_label_set_text(g_status_label, error_buf);
        return;
    }

    lv_label_set_text_static(g_status_label, "Opgeslagen, dashboard vernieuwen...");
    lv_refr_now(NULL);
    if(restart_daemon()) {
        lv_screen_load(g_dashboard_screen);
    }
    else {
        lv_label_set_text_static(g_status_label, "Opgeslagen, maar automatisch verversen is mislukt.");
    }
}

void screen_price_settings_init(lv_obj_t * dashboard_screen)
{
    g_dashboard_screen = dashboard_screen;

    g_settings_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_settings_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_settings_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(g_settings_screen, lv_color_white(), 0);
    lv_obj_set_scrollable(g_settings_screen, false);

    lv_obj_t * title = lv_label_create(g_settings_screen);
    lv_label_set_text_static(title, "Kosten-instellingen");
    lv_obj_set_pos(title, UI_SX(60), UI_SY(6));
    lv_obj_set_style_text_font(title, &lv_font_signage_14, 0);

    /* Same dark-green/black style as the header's hamburger + settings
     * icons - kept in visual sync, see screen_settings.c's identical
     * back/save button styling. */
    lv_obj_t * back_btn = lv_button_create(g_settings_screen);
    lv_obj_set_size(back_btn, UI_SX(44), UI_SY(28));
    lv_obj_set_pos(back_btn, UI_SX(4), UI_SY(2));
    lv_obj_set_style_bg_color(back_btn, bgr_fix(lv_color_make(0, 100, 0)), 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(back_btn, back_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_label = lv_label_create(back_btn);
    lv_label_set_text_static(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
    lv_obj_center(back_label);

    lv_obj_t * save_btn = lv_button_create(g_settings_screen);
    lv_obj_set_size(save_btn, UI_SX(80), UI_SY(28));
    lv_obj_align(save_btn, LV_ALIGN_TOP_RIGHT, -UI_SX(6), UI_SY(2));
    lv_obj_set_style_bg_color(save_btn, bgr_fix(lv_color_make(0, 100, 0)), 0);
    lv_obj_set_style_bg_opa(save_btn, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(save_btn, save_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * save_label = lv_label_create(save_btn);
    lv_label_set_text_static(save_label, "Opslaan");
    lv_obj_set_style_text_font(save_label, &lv_font_signage_14, 0);
    lv_obj_set_style_text_color(save_label, lv_color_white(), 0);
    lv_obj_center(save_label);

    lv_obj_t * enabled_label = lv_label_create(g_settings_screen);
    lv_label_set_text_static(enabled_label, "Gas tonen");
    lv_obj_set_pos(enabled_label, UI_SX(10), UI_SY(46));
    lv_obj_set_style_text_font(enabled_label, &lv_font_signage_14, 0);

    g_enabled_switch = lv_switch_create(g_settings_screen);
    lv_obj_set_pos(g_enabled_switch, UI_SX(170), UI_SY(40));
    /* Green-when-on instead of the default theme's blue/purple-ish knob -
     * same fb_ili9486-scramble-safety reasoning as everywhere else in this
     * app (blue=0 route), and keeps this in visual sync with the rest of
     * the green-on-black scheme. */
    lv_obj_set_style_bg_color(g_enabled_switch, bgr_fix(lv_color_make(0, 150, 0)), LV_PART_INDICATOR | LV_STATE_CHECKED);

    lv_obj_t * unit_label = lv_label_create(g_settings_screen);
    lv_label_set_text_static(unit_label, "Eenheid gas");
    lv_obj_set_pos(unit_label, UI_SX(10), UI_SY(84));
    lv_obj_set_style_text_font(unit_label, &lv_font_signage_14, 0);

    g_unit_dropdown = lv_dropdown_create(g_settings_screen);
    lv_dropdown_set_options_static(g_unit_dropdown, UNIT_OPTIONS);
    lv_obj_set_pos(g_unit_dropdown, UI_SX(170), UI_SY(78));
    lv_obj_set_size(g_unit_dropdown, UI_SX(140), UI_SY(30));
    lv_obj_set_style_bg_color(g_unit_dropdown, bgr_fix(lv_color_make(0, 100, 0)), 0);
    lv_obj_set_style_bg_opa(g_unit_dropdown, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(g_unit_dropdown, lv_color_white(), 0);
    lv_obj_set_style_text_font(g_unit_dropdown, &lv_font_signage_14, 0);

    g_status_label = lv_label_create(g_settings_screen);
    lv_obj_set_pos(g_status_label, UI_SX(10), UI_SY(120));
    lv_obj_set_width(g_status_label, UI_SX(460));
    lv_label_set_long_mode(g_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(g_status_label, &lv_font_signage_14, 0);
}

void screen_price_settings_open(void)
{
    load_current_config();
    lv_screen_load(g_settings_screen);
}
