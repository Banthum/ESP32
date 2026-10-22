#include "screen_battery_settings.h"

#include <stdio.h>
#include <stdlib.h>

#include "cJSON.h"
#include "screen_common.h"
#include "fonts_signage.h"
#include "../core/paths.h"
#include "../daemon/daemon.h"

/* ESP32: LittleFS i.p.v. /etc/emsmachine/ (zie core/paths.h). */
#define BATTERY_CONFIG_PATH BMS_BATTERY_CONFIG_PATH
/* Same Marstek Venus E 3.0 single-unit value as
 * dashboard/battery_config.py's DEFAULT_CAPACITY_KWH and
 * screen_battery.c's DEFAULT_CAPACITY_KWH - shown pre-filled when no
 * config file exists yet, so the field never starts empty. */
#define DEFAULT_CAPACITY_KWH_STR "5.12"
/* Same Marstek Venus E 3.0 single-unit value as
 * dashboard/battery_config.py's DEFAULT_MAX_POWER_W. */
#define DEFAULT_MAX_POWER_W_STR "2500"
/* Same defaults as dashboard/battery_config.py's DEFAULT_MIN_SOC_PCT/
 * DEFAULT_MAX_SOC_PCT/DEFAULT_ENERGY_LOSS_PCT - must match exactly, or
 * this screen's "no config yet" state would show different numbers than
 * what the daemon actually plans with. */
#define DEFAULT_MIN_SOC_PCT_STR "12"
#define DEFAULT_MAX_SOC_PCT_STR "100"
#define DEFAULT_ENERGY_LOSS_PCT_STR "5"

/* Zie clients/marstek_client.h: alleen een LIVE-build stuurt echt aan. */
#ifndef BMS_BATTERY_CONTROL_LIVE
#define BMS_BATTERY_CONTROL_LIVE 0
#endif

static lv_obj_t * g_settings_screen;
static lv_obj_t * g_dashboard_screen;

static lv_obj_t * g_capacity_ta;
static lv_obj_t * g_max_power_ta;
static lv_obj_t * g_max_soc_ta;
static lv_obj_t * g_min_soc_ta;
static lv_obj_t * g_energy_loss_ta;
/* Schakelaar "Batterij aansturen" (active_control_enabled) - ook de snelle
 * noodstop: uit = de ESP32 stuurt niets meer, de batterij valt na het lopende
 * commando (cd_time) zelf terug op Auto. */
static lv_obj_t * g_active_control_sw;
static lv_obj_t * g_status_label;
static lv_obj_t * g_keyboard;

static void back_btn_event_cb(lv_event_t * e)
{
    (void) e;
    lv_screen_load(g_dashboard_screen);
}

/* CLICKED, not FOCUSED - see screen_settings.c's textarea_clicked_cb()
 * for why (LVGL doesn't refire FOCUSED on a tap that lands on an
 * already-focused field once the keyboard's own hide doesn't defocus it). */
static void textarea_clicked_cb(lv_event_t * e)
{
    lv_obj_t * ta = lv_event_get_target_obj(e);
    lv_keyboard_set_textarea(g_keyboard, ta);
    lv_keyboard_set_mode(g_keyboard, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_set_hidden(g_keyboard, false);
}

static void keyboard_ready_cancel_cb(lv_event_t * e)
{
    (void) e;
    lv_obj_set_hidden(g_keyboard, true);
    lv_keyboard_set_textarea(g_keyboard, NULL);
}

/* Loads the current battery_config.json (if any) into the form field -
 * one-shot read when the screen opens, same "no coupling through the IPC
 * protocol for a one-shot value" reasoning as
 * screen_settings.c::load_current_config(). */
static void load_current_config(void)
{
    lv_label_set_text(g_status_label, "");
    lv_textarea_set_text(g_capacity_ta, DEFAULT_CAPACITY_KWH_STR);
    lv_textarea_set_text(g_max_power_ta, DEFAULT_MAX_POWER_W_STR);
    lv_textarea_set_text(g_max_soc_ta, DEFAULT_MAX_SOC_PCT_STR);
    lv_textarea_set_text(g_min_soc_ta, DEFAULT_MIN_SOC_PCT_STR);
    lv_textarea_set_text(g_energy_loss_ta, DEFAULT_ENERGY_LOSS_PCT_STR);
    /* Matches battery_config.DEFAULT_ACTIVE_CONTROL_ENABLED=True. */
    lv_obj_add_state(g_active_control_sw, LV_STATE_CHECKED);

    FILE * f = fopen(BATTERY_CONFIG_PATH, "r");
    if(f == NULL) {
        return; /* no config yet (onboarding not done / step skipped) - leave the defaults shown */
    }
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    cJSON * root = cJSON_Parse(buf);
    if(root == NULL) {
        return;
    }
    cJSON * capacity = cJSON_GetObjectItemCaseSensitive(root, "capacity_kwh");
    if(cJSON_IsNumber(capacity)) {
        char capacity_buf[16];
        snprintf(capacity_buf, sizeof(capacity_buf), "%g", capacity->valuedouble);
        lv_textarea_set_text(g_capacity_ta, capacity_buf);
    }
    cJSON * max_power = cJSON_GetObjectItemCaseSensitive(root, "max_power_w");
    if(cJSON_IsNumber(max_power)) {
        char max_power_buf[16];
        snprintf(max_power_buf, sizeof(max_power_buf), "%g", max_power->valuedouble);
        lv_textarea_set_text(g_max_power_ta, max_power_buf);
    }
    cJSON * active_control = cJSON_GetObjectItemCaseSensitive(root, "active_control_enabled");
    if(cJSON_IsBool(active_control) && !cJSON_IsTrue(active_control)) {
        lv_obj_remove_state(g_active_control_sw, LV_STATE_CHECKED);
    }
    cJSON * max_soc = cJSON_GetObjectItemCaseSensitive(root, "max_soc_pct");
    if(cJSON_IsNumber(max_soc)) {
        char max_soc_buf[16];
        snprintf(max_soc_buf, sizeof(max_soc_buf), "%g", max_soc->valuedouble);
        lv_textarea_set_text(g_max_soc_ta, max_soc_buf);
    }
    cJSON * min_soc = cJSON_GetObjectItemCaseSensitive(root, "min_soc_pct");
    if(cJSON_IsNumber(min_soc)) {
        char min_soc_buf[16];
        snprintf(min_soc_buf, sizeof(min_soc_buf), "%g", min_soc->valuedouble);
        lv_textarea_set_text(g_min_soc_ta, min_soc_buf);
    }
    cJSON * energy_loss = cJSON_GetObjectItemCaseSensitive(root, "energy_loss_pct");
    if(cJSON_IsNumber(energy_loss)) {
        char energy_loss_buf[16];
        snprintf(energy_loss_buf, sizeof(energy_loss_buf), "%g", energy_loss->valuedouble);
        lv_textarea_set_text(g_energy_loss_ta, energy_loss_buf);
    }
    cJSON_Delete(root);
}

/* Writes BATTERY_CONFIG_PATH directly via cJSON - unlike
 * screen_settings.c's Zon-settings screen, there's no external API call
 * needed to persist this (no geocoding-equivalent step), so this skips
 * that screen's posix_spawn'd Python-helper indirection entirely. Returns
 * true on success; on failure *error_out gets a customer-legible reason. */
static bool write_config(double capacity_kwh, double max_power_w, bool active_control_enabled,
                          double min_soc_pct, double max_soc_pct, double energy_loss_pct,
                          char * error_out, size_t error_out_size)
{
    cJSON * root = cJSON_CreateObject();
    if(root == NULL) {
        snprintf(error_out, error_out_size, "interne fout (cJSON)");
        return false;
    }
    cJSON_AddNumberToObject(root, "capacity_kwh", capacity_kwh);
    cJSON_AddNumberToObject(root, "max_power_w", max_power_w);
    cJSON_AddBoolToObject(root, "active_control_enabled", active_control_enabled);
    cJSON_AddNumberToObject(root, "min_soc_pct", min_soc_pct);
    cJSON_AddNumberToObject(root, "max_soc_pct", max_soc_pct);
    cJSON_AddNumberToObject(root, "energy_loss_pct", energy_loss_pct);

    char * json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if(json_str == NULL) {
        snprintf(error_out, error_out_size, "interne fout (cJSON-print)");
        return false;
    }

    FILE * f = fopen(BATTERY_CONFIG_PATH, "w");
    bool ok = false;
    if(f == NULL) {
        snprintf(error_out, error_out_size, "kon %s niet wegschrijven", BATTERY_CONFIG_PATH);
    }
    else {
        ok = fputs(json_str, f) >= 0;
        fclose(f);
        if(!ok) snprintf(error_out, error_out_size, "kon %s niet wegschrijven", BATTERY_CONFIG_PATH);
    }
    cJSON_free(json_str);
    return ok;
}

/* Pi: `systemctl restart dashboard-daemon.service`. ESP32: de daemon draait in
 * hetzelfde proces en leest de instellingen bij de volgende cyclus opnieuw in. */
static bool restart_daemon(void)
{
    daemon_reload_config();
    return true;
}

static void save_btn_event_cb(lv_event_t * e)
{
    (void) e;

    const char * capacity_str = lv_textarea_get_text(g_capacity_ta);
    const char * max_power_str = lv_textarea_get_text(g_max_power_ta);
    const char * max_soc_str = lv_textarea_get_text(g_max_soc_ta);
    const char * min_soc_str = lv_textarea_get_text(g_min_soc_ta);
    const char * energy_loss_str = lv_textarea_get_text(g_energy_loss_ta);
    if(capacity_str[0] == '\0' || max_power_str[0] == '\0' || max_soc_str[0] == '\0'
       || min_soc_str[0] == '\0' || energy_loss_str[0] == '\0') {
        lv_label_set_text_static(g_status_label, "Vul een waarde in");
        return;
    }

    char * endptr;
    double capacity_kwh = strtod(capacity_str, &endptr);
    if(endptr == capacity_str || capacity_kwh <= 0) {
        lv_label_set_text_static(g_status_label, "Ongeldige waarde");
        return;
    }
    double max_power_w = strtod(max_power_str, &endptr);
    if(endptr == max_power_str || max_power_w <= 0) {
        lv_label_set_text_static(g_status_label, "Ongeldige waarde");
        return;
    }
    double max_soc_pct = strtod(max_soc_str, &endptr);
    if(endptr == max_soc_str || max_soc_pct <= 0 || max_soc_pct > 100) {
        lv_label_set_text_static(g_status_label, "Ongeldige waarde");
        return;
    }
    double min_soc_pct = strtod(min_soc_str, &endptr);
    if(endptr == min_soc_str || min_soc_pct < 0 || min_soc_pct >= 100) {
        lv_label_set_text_static(g_status_label, "Ongeldige waarde");
        return;
    }
    double energy_loss_pct = strtod(energy_loss_str, &endptr);
    if(endptr == energy_loss_str || energy_loss_pct < 0 || energy_loss_pct >= 100) {
        lv_label_set_text_static(g_status_label, "Ongeldige waarde");
        return;
    }
    if(min_soc_pct >= max_soc_pct) {
        lv_label_set_text_static(g_status_label, "Min SOC moet lager zijn dan max SOC");
        return;
    }
    bool active_control_enabled = lv_obj_has_state(g_active_control_sw, LV_STATE_CHECKED);

    lv_label_set_text_static(g_status_label, "Bezig met opslaan...");
    lv_refr_now(NULL); /* show the "Bezig..." text before the blocking spawn below */

    char error_buf[128];
    if(!write_config(capacity_kwh, max_power_w, active_control_enabled,
                      min_soc_pct, max_soc_pct, energy_loss_pct, error_buf, sizeof(error_buf))) {
        lv_label_set_text(g_status_label, error_buf);
        return;
    }

    lv_label_set_text_static(g_status_label, "Opgeslagen, dashboard vernieuwen...");
    lv_refr_now(NULL);
    if(restart_daemon()) {
        /* Success: terug naar de Instellingen-pagina (screen_settings_menu.c). */
        lv_screen_load(g_dashboard_screen);
    }
    else {
        lv_label_set_text_static(g_status_label, "Opgeslagen, maar automatisch verversen is mislukt.");
    }
}

void screen_battery_settings_init(lv_obj_t * dashboard_screen)
{
    g_dashboard_screen = dashboard_screen;

    g_settings_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_settings_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_settings_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(g_settings_screen, lv_color_white(), 0);
    lv_obj_set_scrollable(g_settings_screen, false);

    lv_obj_t * title = lv_label_create(g_settings_screen);
    lv_label_set_text_static(title, "Batterij-instellingen");
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

    /* Two-column layout (2026-10-01): 3 rows (same y-positions as the
     * original single-column screen) x 2 columns, needed to fit capacity/
     * max-power/min-max-SOC/energy-loss/active-control into a non-
     * scrollable 480x320 screen whose bottom 180px is permanently reserved
     * for the on-screen keyboard. Field *labels* use the smaller
     * lv_font_signage_10 (not the 14 used elsewhere on this screen) purely
     * so each half-width column has room for Dutch label text - textarea
     * contents/switch stay at the normal 14pt size. Columns are not a
     * generic grid helper since this is the only 2-column settings screen
     * so far; if a third settings screen ever needs this, consider
     * factoring it into screen_common.c then. */
#define ROW1_Y UI_SY(40)
#define ROW2_Y UI_SY(74)
#define ROW3_Y UI_SY(106)
#define LEFT_LABEL_X UI_SX(8)
#define LEFT_TA_X UI_SX(155)
#define COL_TA_W UI_SX(65)
#define RIGHT_LABEL_X UI_SX(240)
#define RIGHT_TA_X UI_SX(395)

    lv_obj_t * capacity_label = lv_label_create(g_settings_screen);
    lv_label_set_text_static(capacity_label, "Capaciteit (kWh)");
    lv_obj_set_pos(capacity_label, LEFT_LABEL_X, ROW1_Y + UI_SY(10));
    lv_obj_set_style_text_font(capacity_label, &lv_font_signage_10, 0);

    g_capacity_ta = lv_textarea_create(g_settings_screen);
    lv_textarea_set_one_line(g_capacity_ta, true);
    lv_textarea_set_max_length(g_capacity_ta, 6);
    lv_textarea_set_accepted_chars_static(g_capacity_ta, "0123456789.");
    lv_obj_set_pos(g_capacity_ta, LEFT_TA_X, ROW1_Y);
    lv_obj_set_size(g_capacity_ta, COL_TA_W, UI_SY(30));
    lv_obj_set_scrollable(g_capacity_ta, false);
    lv_obj_set_style_text_font(g_capacity_ta, &lv_font_signage_14, 0);
    lv_obj_add_event_cb(g_capacity_ta, textarea_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * max_soc_label = lv_label_create(g_settings_screen);
    lv_label_set_text_static(max_soc_label, "Max SOC (%)");
    lv_obj_set_pos(max_soc_label, RIGHT_LABEL_X, ROW1_Y + UI_SY(10));
    lv_obj_set_style_text_font(max_soc_label, &lv_font_signage_10, 0);

    g_max_soc_ta = lv_textarea_create(g_settings_screen);
    lv_textarea_set_one_line(g_max_soc_ta, true);
    lv_textarea_set_max_length(g_max_soc_ta, 5);
    lv_textarea_set_accepted_chars_static(g_max_soc_ta, "0123456789.");
    lv_obj_set_pos(g_max_soc_ta, RIGHT_TA_X, ROW1_Y);
    lv_obj_set_size(g_max_soc_ta, COL_TA_W, UI_SY(30));
    lv_obj_set_scrollable(g_max_soc_ta, false);
    lv_obj_set_style_text_font(g_max_soc_ta, &lv_font_signage_14, 0);
    lv_obj_add_event_cb(g_max_soc_ta, textarea_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * max_power_label = lv_label_create(g_settings_screen);
    lv_label_set_text_static(max_power_label, "Max vermogen (W)");
    lv_obj_set_pos(max_power_label, LEFT_LABEL_X, ROW2_Y + UI_SY(10));
    lv_obj_set_style_text_font(max_power_label, &lv_font_signage_10, 0);

    g_max_power_ta = lv_textarea_create(g_settings_screen);
    lv_textarea_set_one_line(g_max_power_ta, true);
    lv_textarea_set_max_length(g_max_power_ta, 6);
    lv_textarea_set_accepted_chars_static(g_max_power_ta, "0123456789.");
    lv_obj_set_pos(g_max_power_ta, LEFT_TA_X, ROW2_Y);
    lv_obj_set_size(g_max_power_ta, COL_TA_W, UI_SY(30));
    lv_obj_set_scrollable(g_max_power_ta, false);
    lv_obj_set_style_text_font(g_max_power_ta, &lv_font_signage_14, 0);
    lv_obj_add_event_cb(g_max_power_ta, textarea_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * min_soc_label = lv_label_create(g_settings_screen);
    lv_label_set_text_static(min_soc_label, "Min SOC-reserve (%)");
    lv_obj_set_pos(min_soc_label, RIGHT_LABEL_X, ROW2_Y + UI_SY(10));
    lv_obj_set_style_text_font(min_soc_label, &lv_font_signage_10, 0);

    g_min_soc_ta = lv_textarea_create(g_settings_screen);
    lv_textarea_set_one_line(g_min_soc_ta, true);
    lv_textarea_set_max_length(g_min_soc_ta, 5);
    lv_textarea_set_accepted_chars_static(g_min_soc_ta, "0123456789.");
    lv_obj_set_pos(g_min_soc_ta, RIGHT_TA_X, ROW2_Y);
    lv_obj_set_size(g_min_soc_ta, COL_TA_W, UI_SY(30));
    lv_obj_set_scrollable(g_min_soc_ta, false);
    lv_obj_set_style_text_font(g_min_soc_ta, &lv_font_signage_14, 0);
    lv_obj_add_event_cb(g_min_soc_ta, textarea_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * energy_loss_label = lv_label_create(g_settings_screen);
    lv_label_set_text_static(energy_loss_label, "Energieverlies (%)");
    lv_obj_set_pos(energy_loss_label, LEFT_LABEL_X, ROW3_Y + UI_SY(10));
    lv_obj_set_style_text_font(energy_loss_label, &lv_font_signage_10, 0);

    g_energy_loss_ta = lv_textarea_create(g_settings_screen);
    lv_textarea_set_one_line(g_energy_loss_ta, true);
    lv_textarea_set_max_length(g_energy_loss_ta, 5);
    lv_textarea_set_accepted_chars_static(g_energy_loss_ta, "0123456789.");
    lv_obj_set_pos(g_energy_loss_ta, LEFT_TA_X, ROW3_Y);
    lv_obj_set_size(g_energy_loss_ta, COL_TA_W, UI_SY(30));
    lv_obj_set_scrollable(g_energy_loss_ta, false);
    lv_obj_set_style_text_font(g_energy_loss_ta, &lv_font_signage_14, 0);
    lv_obj_add_event_cb(g_energy_loss_ta, textarea_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * active_control_label = lv_label_create(g_settings_screen);
    lv_label_set_text_static(active_control_label,
                             BMS_BATTERY_CONTROL_LIVE ? "Batterij aansturen" : "Aansturen (schaduw-modus)");
    lv_obj_set_pos(active_control_label, RIGHT_LABEL_X, ROW3_Y + UI_SY(10));
    lv_obj_set_style_text_font(active_control_label, &lv_font_signage_10, 0);

    g_active_control_sw = lv_switch_create(g_settings_screen);
    lv_obj_set_pos(g_active_control_sw, RIGHT_TA_X, ROW3_Y + UI_SY(3));
    lv_obj_set_size(g_active_control_sw, UI_SX(50), UI_SY(24));
    /* Zelfde groen als de knoppen (blauw=0, BGR-veilig). */
    lv_obj_set_style_bg_color(g_active_control_sw, bgr_fix(lv_color_make(0, 140, 0)),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);

#undef ROW1_Y
#undef ROW2_Y
#undef ROW3_Y
#undef LEFT_LABEL_X
#undef LEFT_TA_X
#undef COL_TA_W
#undef RIGHT_LABEL_X
#undef RIGHT_TA_X

    g_status_label = lv_label_create(g_settings_screen);
    lv_obj_set_pos(g_status_label, UI_SX(10), UI_SY(136));
    lv_obj_set_width(g_status_label, UI_SX(460));
    lv_label_set_long_mode(g_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(g_status_label, &lv_font_signage_14, 0);

    /* Same "own private keyboard per settings screen" layout as
     * screen_settings.c - this screen only ever needs number mode, but
     * lv_keyboard_create() is cheap and keeping it self-contained avoids
     * any lifetime coupling with the Zon-settings screen's keyboard. */
    g_keyboard = lv_keyboard_create(g_settings_screen);
    lv_obj_set_size(g_keyboard, LV_PCT(100), UI_SY(180));
    lv_obj_align(g_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(g_keyboard, &lv_font_signage_14, 0);
    lv_keyboard_set_mode(g_keyboard, LV_KEYBOARD_MODE_NUMBER);
    /* Same dark-green/black keyboard styling as screen_settings.c's Zon-
     * instellingenscherm (Jeroen, 2026-10-08: wil dit overal consistent) -
     * identical colors/parts/states, see that file's keyboard comment for
     * the full BGR/theme-override rationale. */
    lv_obj_set_style_bg_color(g_keyboard, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_keyboard, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_keyboard, bgr_fix(lv_color_make(0, 100, 0)), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(g_keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(g_keyboard, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(g_keyboard, bgr_fix(lv_color_make(0, 100, 0)), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(g_keyboard, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(g_keyboard, lv_color_white(), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(g_keyboard, keyboard_ready_cancel_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(g_keyboard, keyboard_ready_cancel_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_set_hidden(g_keyboard, true);
}

void screen_battery_settings_open(void)
{
    load_current_config();
    lv_screen_load(g_settings_screen);
}
