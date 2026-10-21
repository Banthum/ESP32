#include "screen_settings.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "screen_common.h"
#include "fonts_signage.h"
#include "../config/solar_settings.h"
#include "../core/paths.h"
#include "../daemon/daemon.h"

/* ESP32: LittleFS i.p.v. /etc/emsmachine/ (zie core/paths.h). */
#define SOLAR_CONFIG_PATH EMS_SOLAR_CONFIG_PATH

/* Same order/keys as display/solar_forecast.py's RICHTING_MAP and
 * solar_settings_writer.py's copy of it - index here must match the
 * dropdown's option order below. */
static const char * const RICHTING_KEYS[] = {"zuid", "zuidoost", "zuidwest", "oost", "west", "plat"};
#define RICHTING_COUNT (sizeof(RICHTING_KEYS) / sizeof(RICHTING_KEYS[0]))
#define RICHTING_OPTIONS "Zuid\nZuidoost\nZuidwest\nOost\nWest\nPlat"
/* Same order as RICHTING_KEYS - only used as this field's *pre-fill*
 * default for a config that doesn't have an explicit "declination" yet
 * (fresh onboarding, or a config written before this field existed).
 * Once saved, "declination" is independently editable - see
 * solar_settings_writer.py's docstring for why (real roof pitch can
 * differ from this per-direction guess). */
static const double RICHTING_DECLINATION_DEFAULTS[] = {35, 35, 35, 35, 35, 15};
/* Same order as RICHTING_KEYS - pre-fill default for "azimuth" only
 * (2026-10-04, same independently-editable pattern as declination above -
 * RICHTING_MAP's coarse 45-degree-step guess doesn't match every real
 * roof, live-caught via comparison against the laptop-VM HA's own PV
 * forecast). Forecast.Solar convention: 0=south, -90=east, 90=west. */
static const double RICHTING_AZIMUTH_DEFAULTS[] = {0, -45, 45, -90, 90, 0};

static lv_obj_t * g_settings_screen;
static lv_obj_t * g_dashboard_screen;

/* Zelfde default als display/onboarding_display.py's STANDARD_PANEL_WP_NL
 * - los gehouden (geen cross-directory import, zie het bestaande "geen
 * koppeling"-patroon in CLAUDE.md), maar bewust dezelfde waarde. */
#define DEFAULT_PANEL_WP 300.0

static lv_obj_t * g_postcode_ta;
static lv_obj_t * g_huisnummer_ta;
static lv_obj_t * g_panel_count_ta;
static lv_obj_t * g_panel_wp_ta;
static lv_obj_t * g_richting_dropdown;
static lv_obj_t * g_factor_ta;
static lv_obj_t * g_declination_ta;
static lv_obj_t * g_azimuth_ta;
static lv_obj_t * g_status_label;
static lv_obj_t * g_keyboard;

/* "Geen zonnepanelen" schuifje (2026-10-13, Jeroen: klanten zonder
 * panelen moeten toch van prijs-arbitrage kunnen profiteren, zonder de
 * verder zinloze postcode/kWp/dakorientatie-velden in te moeten vullen -
 * zelfde doel als onboarding's nieuwe SOLAR_HAS_PANELS_CHOICE-stap, zie
 * display/onboarding_display.py). Elk veld/label hieronder dat alleen
 * relevant is als er wél panelen zijn staat in g_panel_fields, zodat één
 * loop ze allemaal tegelijk kan verbergen/tonen - zie
 * set_panel_fields_hidden(). */
static lv_obj_t * g_no_panels_switch;
#define PANEL_FIELD_COUNT 20
static lv_obj_t * g_panel_fields[PANEL_FIELD_COUNT];
static int g_panel_field_count;

static void register_panel_field(lv_obj_t * obj)
{
    g_panel_fields[g_panel_field_count++] = obj;
}

static void set_panel_fields_hidden(bool hidden)
{
    for(int i = 0; i < g_panel_field_count; i++) {
        lv_obj_set_hidden(g_panel_fields[i], hidden);
    }
}

static void back_btn_event_cb(lv_event_t * e)
{
    (void) e;
    lv_screen_load(g_dashboard_screen);
}

/* LV_EVENT_CLICKED, not LV_EVENT_FOCUSED: this app has no lv_group/encoder
 * input, only touch, and LVGL doesn't refire FOCUSED on a tap that lands
 * on an already-focused object - closing the keyboard (see
 * keyboard_ready_cancel_cb()) never actually defocuses the textarea, so a
 * FOCUSED-based handler stopped reopening the keyboard on the 2nd tap of
 * the same field (live-confirmed). CLICKED fires on every tap regardless
 * of prior focus state. */
static void textarea_clicked_cb(lv_event_t * e)
{
    lv_obj_t * ta = lv_event_get_target_obj(e);
    lv_keyboard_set_textarea(g_keyboard, ta);
    bool numeric = ta == g_panel_count_ta || ta == g_panel_wp_ta;
    lv_keyboard_set_mode(g_keyboard, numeric ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_hidden(g_keyboard, false);
}

static void keyboard_ready_cancel_cb(lv_event_t * e)
{
    (void) e;
    lv_obj_set_hidden(g_keyboard, true);
    lv_keyboard_set_textarea(g_keyboard, NULL);
}

/* Live tonen/verbergen zodra het schuifje omgaat (niet pas na "Opslaan")
 * - zelfde moment-van-feedback als elk ander veld op dit scherm. Verbergt
 * ook het toetsenbord, voor het geval dat nog open stond toen het
 * schuifje werd omgezet - er is dan geen enkel tekstveld meer zichtbaar
 * om het aan te koppelen. */
static void no_panels_switch_event_cb(lv_event_t * e)
{
    (void) e;
    bool no_panels = lv_obj_has_state(g_no_panels_switch, LV_STATE_CHECKED);
    set_panel_fields_hidden(no_panels);
    if(no_panels) {
        lv_obj_set_hidden(g_keyboard, true);
        lv_keyboard_set_textarea(g_keyboard, NULL);
    }
}

/* Loads the current solar_config.json (if any) into the form fields.
 * Read directly in C via cJSON rather than piping every field through the
 * IPC protocol from the daemon (see CLAUDE.md's "no coupling" style) -
 * this is a one-shot read when the screen opens, not a polled value. */
static void load_current_config(void)
{
    lv_label_set_text(g_status_label, "");
    lv_textarea_set_text(g_factor_ta, "1.0"); /* default even if no config file exists yet */
    char default_wp_buf[16];
    snprintf(default_wp_buf, sizeof(default_wp_buf), "%g", DEFAULT_PANEL_WP);
    lv_textarea_set_text(g_panel_wp_ta, default_wp_buf); /* same idea - Aantal panelen has no sane default */
    char default_dec_buf[16];
    snprintf(default_dec_buf, sizeof(default_dec_buf), "%g", RICHTING_DECLINATION_DEFAULTS[0]); /* "zuid" */
    lv_textarea_set_text(g_declination_ta, default_dec_buf);
    char default_az_buf[16];
    snprintf(default_az_buf, sizeof(default_az_buf), "%g", RICHTING_AZIMUTH_DEFAULTS[0]); /* "zuid" */
    lv_textarea_set_text(g_azimuth_ta, default_az_buf);

    /* Default: schuifje uit, panelen-velden zichtbaar - zelfde gedrag als
     * vóór dit schuifje bestond voor elke config zonder (of met
     * no_solar_panels=false) deze sleutel. */
    lv_obj_remove_state(g_no_panels_switch, LV_STATE_CHECKED);
    set_panel_fields_hidden(false);

    FILE * f = fopen(SOLAR_CONFIG_PATH, "r");
    if(f == NULL) {
        return; /* onboarding not done yet / solar step skipped - leave fields empty */
    }
    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    cJSON * root = cJSON_Parse(buf);
    if(root == NULL) {
        return;
    }

    cJSON * no_solar_panels = cJSON_GetObjectItemCaseSensitive(root, "no_solar_panels");
    if(cJSON_IsBool(no_solar_panels) && cJSON_IsTrue(no_solar_panels)) {
        lv_obj_add_state(g_no_panels_switch, LV_STATE_CHECKED);
        set_panel_fields_hidden(true);
        /* Geen van de overige velden (postcode/kwp/richting/...) bestaat
         * in zo'n config - niets om verder uit te lezen. */
        cJSON_Delete(root);
        return;
    }

    cJSON * postcode = cJSON_GetObjectItemCaseSensitive(root, "postcode");
    if(cJSON_IsString(postcode)) lv_textarea_set_text(g_postcode_ta, postcode->valuestring);

    cJSON * huisnummer = cJSON_GetObjectItemCaseSensitive(root, "huisnummer");
    if(cJSON_IsString(huisnummer)) lv_textarea_set_text(g_huisnummer_ta, huisnummer->valuestring);

    /* Prefer the explicit panel_count/panel_wp pair (written by this
     * screen itself, see solar_settings_writer.py) so re-opening settings
     * shows back exactly what was typed. Older configs (written before
     * this field split existed, or via onboarding's "vul zelf de kWp in"
     * path) only have "kwp" - fall back to DEFAULT_PANEL_WP and derive a
     * plausible panel_count from that, rather than leaving both fields
     * empty for a customer who already has a working config. */
    cJSON * panel_count = cJSON_GetObjectItemCaseSensitive(root, "panel_count");
    cJSON * panel_wp = cJSON_GetObjectItemCaseSensitive(root, "panel_wp");
    if(cJSON_IsNumber(panel_count) && cJSON_IsNumber(panel_wp)) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", (int) panel_count->valuedouble);
        lv_textarea_set_text(g_panel_count_ta, buf);
        snprintf(buf, sizeof(buf), "%g", panel_wp->valuedouble);
        lv_textarea_set_text(g_panel_wp_ta, buf);
    }
    else {
        cJSON * kwp = cJSON_GetObjectItemCaseSensitive(root, "kwp");
        if(cJSON_IsNumber(kwp)) {
            char buf[16];
            int derived_count = (int) (kwp->valuedouble * 1000.0 / DEFAULT_PANEL_WP + 0.5);
            if(derived_count < 1) derived_count = 1;
            snprintf(buf, sizeof(buf), "%d", derived_count);
            lv_textarea_set_text(g_panel_count_ta, buf);
            snprintf(buf, sizeof(buf), "%g", DEFAULT_PANEL_WP);
            lv_textarea_set_text(g_panel_wp_ta, buf);
        }
    }

    uint32_t richting_idx_for_default = 0; /* "zuid" - used below if declination is absent */
    cJSON * richting = cJSON_GetObjectItemCaseSensitive(root, "richting");
    if(cJSON_IsString(richting)) {
        for(size_t i = 0; i < RICHTING_COUNT; i++) {
            if(strcmp(richting->valuestring, RICHTING_KEYS[i]) == 0) {
                lv_dropdown_set_selected(g_richting_dropdown, (uint32_t) i);
                richting_idx_for_default = (uint32_t) i;
                break;
            }
        }
    }

    /* Independently editable since 2026-10-03 (see solar_settings_writer.py's
     * docstring) - falls back to RICHTING_MAP's per-direction guess only
     * when no explicit value has ever been saved, same "sane pre-fill, not
     * an empty field to figure out" idiom as forecast_factor's "1.0". */
    cJSON * declination = cJSON_GetObjectItemCaseSensitive(root, "declination");
    if(cJSON_IsNumber(declination)) {
        char dec_buf[16];
        snprintf(dec_buf, sizeof(dec_buf), "%g", declination->valuedouble);
        lv_textarea_set_text(g_declination_ta, dec_buf);
    }
    else {
        char dec_buf[16];
        snprintf(dec_buf, sizeof(dec_buf), "%g", RICHTING_DECLINATION_DEFAULTS[richting_idx_for_default]);
        lv_textarea_set_text(g_declination_ta, dec_buf);
    }

    /* Same independently-editable pattern as declination above. */
    cJSON * azimuth = cJSON_GetObjectItemCaseSensitive(root, "azimuth");
    if(cJSON_IsNumber(azimuth)) {
        char az_buf[16];
        snprintf(az_buf, sizeof(az_buf), "%g", azimuth->valuedouble);
        lv_textarea_set_text(g_azimuth_ta, az_buf);
    }
    else {
        char az_buf[16];
        snprintf(az_buf, sizeof(az_buf), "%g", RICHTING_AZIMUTH_DEFAULTS[richting_idx_for_default]);
        lv_textarea_set_text(g_azimuth_ta, az_buf);
    }

    /* Absent (config predates this field, or a fresh onboarding run) ->
     * "1.0" (no change to the raw Forecast.Solar estimate) rather than an
     * empty field the customer has to figure out how to fill in. */
    cJSON * factor = cJSON_GetObjectItemCaseSensitive(root, "forecast_factor");
    if(cJSON_IsNumber(factor)) {
        char factor_buf[16];
        snprintf(factor_buf, sizeof(factor_buf), "%g", factor->valuedouble);
        lv_textarea_set_text(g_factor_ta, factor_buf);
    }
    else {
        lv_textarea_set_text(g_factor_ta, "1.0");
    }

    cJSON_Delete(root);
}

/* Runs solar_settings_writer.py with a real argv array (posix_spawn, no
 * shell involved) so free-text postcode/huisnummer from the on-screen
 * keyboard can never reach a shell as a metacharacter - see
 * solar_settings_writer.py's module docstring for why this matters.
 * Blocks the LVGL thread for the duration of the PDOK geocoding call
 * (up to REQUEST_TIMEOUT_S there) - acceptable for this rare, explicit,
 * user-initiated action in this single-threaded app (same trade-off the
 * rest of this app's synchronous design already makes elsewhere). Returns
 * true on success; on failure, *error_out is set to the child's captured
 * stderr (customer-legible reason text, not a raw trace - see the writer
 * script's own docstring). */
/* Pi: spawnt solar_settings_writer.py. ESP32: dezelfde validatie + PDOK-
 * geocoding + wegschrijven in config/solar_settings.cpp. Blokkeert de
 * LVGL-taak tijdens de PDOK-call, net als op de Pi. */
static bool run_writer_script(const char * postcode, const char * huisnummer, const char * panel_count_str,
                               const char * panel_wp_str, const char * richting, const char * factor_str,
                               const char * declination_str, const char * azimuth_str, char * error_out,
                               size_t error_out_size)
{
    error_out[0] = '\0';
    return solar_settings_write(postcode, huisnummer, panel_count_str, panel_wp_str, richting, factor_str,
                                declination_str, azimuth_str, error_out, error_out_size);
}

/* Pi: `systemctl restart dashboard-daemon.service` (verse zon-fetch). ESP32:
 * de daemon leest opnieuw in en haalt de voorspelling meteen opnieuw op. */
static bool restart_solar_daemon(void)
{
    daemon_reload_config();
    return true;
}

/* "Geen zonnepanelen" opslaan - volledige overschrijving, zie
 * solar_settings_write_no_panels(). */
static bool write_no_solar_panels_config(void)
{
    char error_buf[96];
    return solar_settings_write_no_panels(error_buf, sizeof(error_buf));
}

static void save_btn_event_cb(lv_event_t * e)
{
    (void) e;

    if(lv_obj_has_state(g_no_panels_switch, LV_STATE_CHECKED)) {
        lv_label_set_text_static(g_status_label, "Bezig met opslaan...");
        lv_refr_now(NULL);
        if(!write_no_solar_panels_config()) {
            lv_label_set_text_static(g_status_label, "Opslaan mislukt");
            return;
        }
        lv_label_set_text_static(g_status_label, "Opgeslagen, voorspelling vernieuwen...");
        lv_refr_now(NULL);
        if(restart_solar_daemon()) {
            lv_screen_load(g_dashboard_screen);
        }
        else {
            lv_label_set_text_static(g_status_label, "Opgeslagen, maar automatisch verversen is mislukt.");
        }
        return;
    }

    const char * postcode = lv_textarea_get_text(g_postcode_ta);
    const char * huisnummer = lv_textarea_get_text(g_huisnummer_ta);
    const char * panel_count_str = lv_textarea_get_text(g_panel_count_ta);
    const char * panel_wp_str = lv_textarea_get_text(g_panel_wp_ta);
    uint32_t richting_idx = lv_dropdown_get_selected(g_richting_dropdown);
    const char * factor_str = lv_textarea_get_text(g_factor_ta);
    const char * declination_str = lv_textarea_get_text(g_declination_ta);
    const char * azimuth_str = lv_textarea_get_text(g_azimuth_ta);

    if(postcode[0] == '\0' || huisnummer[0] == '\0' || panel_count_str[0] == '\0' || panel_wp_str[0] == '\0' ||
       factor_str[0] == '\0' || declination_str[0] == '\0' || azimuth_str[0] == '\0') {
        lv_label_set_text_static(g_status_label, "Vul alle velden in");
        return;
    }

    lv_label_set_text_static(g_status_label, "Bezig met opslaan...");
    lv_refr_now(NULL); /* show the "Bezig..." text before the blocking spawn below */

    char error_buf[256];
    bool ok = run_writer_script(postcode, huisnummer, panel_count_str, panel_wp_str, RICHTING_KEYS[richting_idx],
                                 factor_str, declination_str, azimuth_str, error_buf, sizeof(error_buf));
    if(!ok) {
        lv_label_set_text(g_status_label, error_buf[0] != '\0' ? error_buf : "Opslaan mislukt");
        return;
    }

    lv_label_set_text_static(g_status_label, "Opgeslagen, voorspelling vernieuwen...");
    lv_refr_now(NULL);
    if(restart_solar_daemon()) {
        /* Success: go straight back to the dashboard (the Zon tab, since
         * that's the only way to reach this screen) instead of leaving
         * the customer looking at a status message they'd have to
         * manually navigate away from. */
        lv_screen_load(g_dashboard_screen);
    }
    else {
        /* Failure: stay here so whoever's configuring this notices
         * something needs attention, instead of silently going back to a
         * dashboard that's still showing the old forecast. */
        lv_label_set_text_static(g_status_label, "Opgeslagen, maar automatisch verversen is mislukt.");
    }
}

static lv_obj_t * add_field_label(lv_obj_t * parent, const char * text, int32_t x, int32_t y)
{
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text_static(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, &lv_font_signage_14, 0);
    return label;
}

void screen_settings_init(lv_obj_t * dashboard_screen)
{
    g_dashboard_screen = dashboard_screen;

    g_settings_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_settings_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_settings_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(g_settings_screen, lv_color_white(), 0);
    lv_obj_set_scrollable(g_settings_screen, false);

    lv_obj_t * title = lv_label_create(g_settings_screen);
    lv_label_set_text_static(title, "Zon-instellingen");
    lv_obj_set_pos(title, UI_SX(60), UI_SY(6));
    lv_obj_set_style_text_font(title, &lv_font_signage_14, 0);

    /* Same dark-green/black style as the header's hamburger + settings
     * icons (dashboard_ui.c's build_header()) - kept in visual sync since
     * they're all the same kind of icon/action button. Default theme
     * button color (a purple-ish blue) otherwise looked inconsistent with
     * the rest of the app's green-on-black scheme (live-observed). */
    lv_obj_t * back_btn = lv_button_create(g_settings_screen);
    lv_obj_set_size(back_btn, UI_SX(44), UI_SY(28));
    lv_obj_set_pos(back_btn, UI_SX(4), UI_SY(2));
    lv_obj_set_style_bg_color(back_btn, bgr_fix(lv_color_make(0, 100, 0)), 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(back_btn, back_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_label = lv_label_create(back_btn);
    lv_label_set_text_static(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0); /* contrast on dark green */
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
    lv_obj_set_style_text_color(save_label, lv_color_white(), 0); /* contrast on dark green */
    lv_obj_center(save_label);

    /* Moved into the header row (2026-10-03) to free up its old slot
     * (250, 108) in the 2-column field grid below for the new Kantelhoek
     * field - the header has open space between the title and save
     * button at this screen's current title length. Still wraps
     * (LV_LABEL_LONG_WRAP) for longer messages like the save-failure
     * reason text. */
    g_status_label = lv_label_create(g_settings_screen);
    lv_obj_set_pos(g_status_label, UI_SX(200), UI_SY(10));
    lv_obj_set_width(g_status_label, UI_SX(180));
    lv_label_set_long_mode(g_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(g_status_label, &lv_font_signage_14, 0);

    /* Two columns, everything above y=140 - the on-screen keyboard below
     * docks at the bottom 180px (BOTTOM_MID, see g_keyboard below), so
     * anything lower would be hidden behind it while editing any field. */
    register_panel_field(add_field_label(g_settings_screen, "Postcode", UI_SX(10), UI_SY(40)));
    g_postcode_ta = lv_textarea_create(g_settings_screen);
    register_panel_field(g_postcode_ta);
    lv_textarea_set_one_line(g_postcode_ta, true);
    lv_textarea_set_max_length(g_postcode_ta, 6);
    lv_obj_set_pos(g_postcode_ta, UI_SX(90), UI_SY(36));
    lv_obj_set_size(g_postcode_ta, UI_SX(100), UI_SY(30));
    lv_obj_set_scrollable(g_postcode_ta, false);
    lv_obj_set_style_text_font(g_postcode_ta, &lv_font_signage_14, 0);
    lv_obj_add_event_cb(g_postcode_ta, textarea_clicked_cb, LV_EVENT_CLICKED, NULL);

    register_panel_field(add_field_label(g_settings_screen, "Huisnr.", UI_SX(10), UI_SY(76)));
    g_huisnummer_ta = lv_textarea_create(g_settings_screen);
    register_panel_field(g_huisnummer_ta);
    lv_textarea_set_one_line(g_huisnummer_ta, true);
    lv_textarea_set_max_length(g_huisnummer_ta, 8);
    lv_obj_set_pos(g_huisnummer_ta, UI_SX(90), UI_SY(72));
    lv_obj_set_size(g_huisnummer_ta, UI_SX(100), UI_SY(30));
    lv_obj_set_scrollable(g_huisnummer_ta, false);
    lv_obj_set_style_text_font(g_huisnummer_ta, &lv_font_signage_14, 0);
    lv_obj_add_event_cb(g_huisnummer_ta, textarea_clicked_cb, LV_EVENT_CLICKED, NULL);

    /* Vervangt de vroegere enkele "kWp"-invoer (op verzoek, 2026-09-30):
     * klant vult aantal panelen + Wp per paneel in, kWp wordt hieruit
     * afgeleid door solar_settings_writer.py - zelfde twee-getallen-model
     * als onboarding's paneel-aantal-pad (STANDARD_PANEL_WP_NL, zie
     * display/onboarding_display.py) i.p.v. een kale kWp-schatting die de
     * klant zelf moet uitrekenen/opzoeken. Beide velden naast elkaar in
     * dezelfde rij (i.p.v. een eigen rij per veld) - er is geen ruimte
     * over voor een 4e rij in deze kolom boven de y=140-grens waar het
     * toetsenbord begint (zie de kolomcomment hierboven). */
    register_panel_field(add_field_label(g_settings_screen, "Panelen", UI_SX(10), UI_SY(112)));
    g_panel_count_ta = lv_textarea_create(g_settings_screen);
    register_panel_field(g_panel_count_ta);
    lv_textarea_set_one_line(g_panel_count_ta, true);
    lv_textarea_set_max_length(g_panel_count_ta, 3);
    lv_textarea_set_accepted_chars_static(g_panel_count_ta, "0123456789");
    lv_obj_set_pos(g_panel_count_ta, UI_SX(90), UI_SY(108));
    lv_obj_set_size(g_panel_count_ta, UI_SX(45), UI_SY(30));
    lv_obj_set_scrollable(g_panel_count_ta, false);
    lv_obj_set_style_text_font(g_panel_count_ta, &lv_font_signage_14, 0);
    lv_obj_add_event_cb(g_panel_count_ta, textarea_clicked_cb, LV_EVENT_CLICKED, NULL);

    register_panel_field(add_field_label(g_settings_screen, "x", UI_SX(140), UI_SY(112)));
    g_panel_wp_ta = lv_textarea_create(g_settings_screen);
    register_panel_field(g_panel_wp_ta);
    lv_textarea_set_one_line(g_panel_wp_ta, true);
    lv_textarea_set_max_length(g_panel_wp_ta, 4);
    lv_textarea_set_accepted_chars_static(g_panel_wp_ta, "0123456789");
    lv_obj_set_pos(g_panel_wp_ta, UI_SX(155), UI_SY(108));
    lv_obj_set_size(g_panel_wp_ta, UI_SX(55), UI_SY(30));
    lv_obj_set_scrollable(g_panel_wp_ta, false);
    lv_obj_set_style_text_font(g_panel_wp_ta, &lv_font_signage_14, 0);
    lv_obj_add_event_cb(g_panel_wp_ta, textarea_clicked_cb, LV_EVENT_CLICKED, NULL);
    register_panel_field(add_field_label(g_settings_screen, "Wp", UI_SX(215), UI_SY(112)));

    register_panel_field(add_field_label(g_settings_screen, "Richting", UI_SX(250), UI_SY(40)));
    g_richting_dropdown = lv_dropdown_create(g_settings_screen);
    register_panel_field(g_richting_dropdown);
    lv_dropdown_set_options_static(g_richting_dropdown, RICHTING_OPTIONS);
    lv_obj_set_pos(g_richting_dropdown, UI_SX(330), UI_SY(36));
    /* Narrowed from 140 to 100 (2026-10-04) to make room for the new
     * azimuth field right next to it on the same row - richting now only
     * sets the *pre-fill default*, azimuth itself is the independently-
     * editable value actually used (see solar_settings_writer.py's
     * docstring), so a full-width dropdown isn't as load-bearing as it
     * used to be. */
    lv_obj_set_size(g_richting_dropdown, UI_SX(100), UI_SY(30));
    lv_obj_set_style_bg_color(g_richting_dropdown, bgr_fix(lv_color_make(0, 100, 0)), 0);
    lv_obj_set_style_bg_opa(g_richting_dropdown, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(g_richting_dropdown, lv_color_white(), 0); /* contrast on dark green */
    lv_obj_set_style_text_font(g_richting_dropdown, &lv_font_signage_14, 0);

    /* Azimuth (graden, Forecast.Solar-conventie: 0=zuid, -90=oost,
     * 90=west) - independently editable since 2026-10-04, zelfde patroon
     * als Kantelhoek (zie RICHTING_AZIMUTH_DEFAULTS' comment). Krap naast
     * de versmalde Richting-dropdown op dezelfde rij, geen losse
     * eenheid-label (geen ruimte) - het veld zelf heet al "Azimuth". */
    g_azimuth_ta = lv_textarea_create(g_settings_screen);
    register_panel_field(g_azimuth_ta);
    lv_textarea_set_one_line(g_azimuth_ta, true);
    lv_textarea_set_max_length(g_azimuth_ta, 6);
    lv_textarea_set_accepted_chars_static(g_azimuth_ta, "0123456789.-");
    lv_obj_set_pos(g_azimuth_ta, UI_SX(434), UI_SY(36));
    lv_obj_set_size(g_azimuth_ta, UI_SX(36), UI_SY(30));
    lv_obj_set_scrollable(g_azimuth_ta, false);
    lv_obj_set_style_text_font(g_azimuth_ta, &lv_font_signage_14, 0);
    lv_obj_add_event_cb(g_azimuth_ta, textarea_clicked_cb, LV_EVENT_CLICKED, NULL);

    /* Customer-adjustable multiplier on the raw Forecast.Solar estimate -
     * see dashboard_daemon.py's use of "forecast_factor". 1.0 = unchanged. */
    register_panel_field(add_field_label(g_settings_screen, "Kalibratie", UI_SX(250), UI_SY(76)));
    g_factor_ta = lv_textarea_create(g_settings_screen);
    register_panel_field(g_factor_ta);
    lv_textarea_set_one_line(g_factor_ta, true);
    lv_textarea_set_max_length(g_factor_ta, 5);
    lv_textarea_set_accepted_chars_static(g_factor_ta, "0123456789.");
    lv_obj_set_pos(g_factor_ta, UI_SX(330), UI_SY(72));
    lv_obj_set_size(g_factor_ta, UI_SX(100), UI_SY(30));
    lv_obj_set_scrollable(g_factor_ta, false);
    lv_obj_set_style_text_font(g_factor_ta, &lv_font_signage_14, 0);
    lv_obj_add_event_cb(g_factor_ta, textarea_clicked_cb, LV_EVENT_CLICKED, NULL);

    /* Kantelhoek (declination, graden) - independently editable since
     * 2026-10-03, see RICHTING_DECLINATION_DEFAULTS's comment and
     * solar_settings_writer.py's docstring for why (RICHTING_MAP's fixed
     * per-direction guess doesn't match every real roof pitch). Takes the
     * old status-label slot in this row/column - g_status_label moved to
     * the header row above. */
    register_panel_field(add_field_label(g_settings_screen, "Kantelhoek", UI_SX(250), UI_SY(112)));
    g_declination_ta = lv_textarea_create(g_settings_screen);
    register_panel_field(g_declination_ta);
    lv_textarea_set_one_line(g_declination_ta, true);
    lv_textarea_set_max_length(g_declination_ta, 5);
    lv_textarea_set_accepted_chars_static(g_declination_ta, "0123456789.");
    lv_obj_set_pos(g_declination_ta, UI_SX(340), UI_SY(108));
    lv_obj_set_size(g_declination_ta, UI_SX(60), UI_SY(30));
    lv_obj_set_scrollable(g_declination_ta, false);
    lv_obj_set_style_text_font(g_declination_ta, &lv_font_signage_14, 0);
    lv_obj_add_event_cb(g_declination_ta, textarea_clicked_cb, LV_EVENT_CLICKED, NULL);
    register_panel_field(add_field_label(g_settings_screen, "graden", UI_SX(405), UI_SY(112)));

    /* "Geen zonnepanelen"-schuifje, los van de bovenstaande velden - zie
     * de g_panel_fields-comment hierboven. Geen keyboard-interactie nodig
     * (het is een switch), dus geen probleem dat dit net onder de y=140-
     * grens zit waar het toetsenbord anders zou kunnen overlappen - dat
     * toetsenbord is per definitie verborgen zodra dit schuifje relevant
     * is (er is dan geen enkel tekstveld meer zichtbaar om te focussen). */
    lv_obj_t * no_panels_label = lv_label_create(g_settings_screen);
    lv_label_set_text_static(no_panels_label, "Geen zonnepanelen");
    lv_obj_set_pos(no_panels_label, UI_SX(10), UI_SY(150));
    lv_obj_set_style_text_font(no_panels_label, &lv_font_signage_14, 0);

    g_no_panels_switch = lv_switch_create(g_settings_screen);
    lv_obj_set_pos(g_no_panels_switch, UI_SX(160), UI_SY(144));
    lv_obj_set_style_bg_color(g_no_panels_switch, bgr_fix(lv_color_make(0, 150, 0)), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(g_no_panels_switch, no_panels_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    g_keyboard = lv_keyboard_create(g_settings_screen);
    lv_obj_set_size(g_keyboard, LV_PCT(100), UI_SY(180));
    lv_obj_align(g_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(g_keyboard, &lv_font_signage_14, 0);
    /* Default LVGL theme leaves this a plain white-keys/light-grey-bg
     * matrix (live-photographed 2026-09-30, Jeroen flagged it as "doesn't
     * look right yet") - stood out against every other control on this
     * screen (back/save buttons, richting dropdown) already explicitly
     * themed dark-green/black on request, see those widgets' own comments
     * above. Matches the same styling here: black gap background, dark
     * green keys, white key labels. (0,100,0) is the same blue=0 scramble-
     * safe color already used elsewhere in this file, see CLAUDE.md's
     * fb_ili9486 color-bug section - no new risk introduced.) */
    lv_obj_set_style_bg_color(g_keyboard, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_keyboard, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_keyboard, bgr_fix(lv_color_make(0, 100, 0)), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(g_keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(g_keyboard, lv_color_white(), LV_PART_ITEMS);
    /* The theme separately styles "special" keys (backspace, ABC/123
     * toggle, enter, space, arrows) with LV_PART_ITEMS | LV_STATE_CHECKED
     * (see lv_theme_default.c's keyboard block, theme->styles.bg_color_grey
     * on that exact part+state combo) - more specific than the plain
     * LV_PART_ITEMS override above, so it wins there and those keys stayed
     * default-theme grey without this. */
    lv_obj_set_style_bg_color(g_keyboard, bgr_fix(lv_color_make(0, 100, 0)), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(g_keyboard, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(g_keyboard, lv_color_white(), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(g_keyboard, keyboard_ready_cancel_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(g_keyboard, keyboard_ready_cancel_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_set_hidden(g_keyboard, true);
}

void screen_settings_open(void)
{
    load_current_config();
    lv_screen_load(g_settings_screen);
}
