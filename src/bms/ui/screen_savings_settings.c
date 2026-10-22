#include "screen_savings_settings.h"

#include <stdio.h>
#include <string.h>

#include "screen_common.h"
#include "fonts_signage.h"
#include "../daemon/daemon.h"
#include "../storage/storage.h"

static lv_obj_t * g_settings_screen;
static lv_obj_t * g_dashboard_screen;

static lv_obj_t * g_reset_btn;
static lv_obj_t * g_confirm_yes_btn;
static lv_obj_t * g_confirm_no_btn;
static lv_obj_t * g_confirm_label;
static lv_obj_t * g_status_label;

static void back_btn_event_cb(lv_event_t * e)
{
    (void) e;
    lv_screen_load(g_dashboard_screen);
}

/* Two-step reveal instead of a single "reset" tap - this permanently
 * deletes every historical savings row, no undo (see
 * savings_reset_writer.py). screen_common_set_hidden() flips the two
 * button-states; no separate confirm-screen needed since both states fit
 * comfortably on this one screen. */
static void show_confirm_state(bool confirming)
{
    screen_common_set_hidden(g_reset_btn, confirming);
    screen_common_set_hidden(g_confirm_label, !confirming);
    screen_common_set_hidden(g_confirm_yes_btn, !confirming);
    screen_common_set_hidden(g_confirm_no_btn, !confirming);
}

static void reset_btn_event_cb(lv_event_t * e)
{
    (void) e;
    lv_label_set_text(g_status_label, "");
    show_confirm_state(true);
}

static void cancel_btn_event_cb(lv_event_t * e)
{
    (void) e;
    show_confirm_state(false);
}

/* Pi: spawnt savings_reset_writer.py. ESP32: dezelfde twee DELETE's direct
 * via storage (zie storage_reset_savings()). */
static bool run_reset_writer(char * error_out, size_t error_out_size)
{
    return storage_reset_savings(error_out, (int) error_out_size);
}

/* Zonder dit zou de daemon z'n in-memory shadow-SOC/tellers bij de volgende
 * flush weer terugschrijven (op de Pi: daemon-herstart). */
static bool restart_daemon(void)
{
    daemon_reload_config();
    return true;
}

static void confirm_yes_btn_event_cb(lv_event_t * e)
{
    (void) e;

    lv_label_set_text_static(g_status_label, "Bezig met resetten...");
    lv_refr_now(NULL); /* show the "Bezig..." text before the blocking spawn below */

    char error_buf[160];
    if(!run_reset_writer(error_buf, sizeof(error_buf))) {
        lv_label_set_text(g_status_label, error_buf);
        show_confirm_state(false);
        return;
    }

    lv_label_set_text_static(g_status_label, "Gereset, dashboard vernieuwen...");
    lv_refr_now(NULL);
    if(restart_daemon()) {
        lv_screen_load(g_dashboard_screen);
    }
    else {
        lv_label_set_text_static(g_status_label, "Gereset, maar automatisch verversen is mislukt.");
        show_confirm_state(false);
    }
}

void screen_savings_settings_init(lv_obj_t * dashboard_screen)
{
    g_dashboard_screen = dashboard_screen;

    g_settings_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_settings_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_settings_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(g_settings_screen, lv_color_white(), 0);
    lv_obj_set_scrollable(g_settings_screen, false);

    lv_obj_t * title = lv_label_create(g_settings_screen);
    lv_label_set_text_static(title, "Besparing-instellingen");
    lv_obj_set_pos(title, UI_SX(60), UI_SY(6));
    lv_obj_set_style_text_font(title, &lv_font_signage_14, 0);

    /* Same dark-green/black back-button style as the other settings
     * screens - no "Opslaan" button here, there's nothing to save, only
     * reset. */
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

    lv_obj_t * explain_label = lv_label_create(g_settings_screen);
    lv_label_set_text_static(
        explain_label,
        "Dit wist alle opgebouwde besparingsdata (dag/week/maand/jaar/altijd)\n"
        "permanent. Dit kan niet ongedaan gemaakt worden.");
    lv_obj_set_pos(explain_label, UI_SX(10), UI_SY(50));
    lv_obj_set_width(explain_label, UI_SX(460));
    lv_label_set_long_mode(explain_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(explain_label, &lv_font_signage_14, 0);

    /* Initial state: one plain button. Tapping it only reveals the
     * confirm step below (show_confirm_state()), it doesn't reset
     * anything itself - neutral green, same as every other button on
     * this app, since this tap alone isn't yet destructive. */
    g_reset_btn = lv_button_create(g_settings_screen);
    lv_obj_set_size(g_reset_btn, UI_SX(200), UI_SY(36));
    lv_obj_set_pos(g_reset_btn, UI_SX(10), UI_SY(110));
    lv_obj_set_style_bg_color(g_reset_btn, bgr_fix(lv_color_make(0, 100, 0)), 0);
    lv_obj_set_style_bg_opa(g_reset_btn, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(g_reset_btn, reset_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * reset_label = lv_label_create(g_reset_btn);
    lv_label_set_text_static(reset_label, "Tellers resetten");
    lv_obj_set_style_text_font(reset_label, &lv_font_signage_14, 0);
    lv_obj_set_style_text_color(reset_label, lv_color_white(), 0);
    lv_obj_center(reset_label);

    /* Confirm step - hidden until the button above is tapped. Red for the
     * actually-destructive action (bgr_fix-safe: blue=0), green for
     * backing out, same red/green warning-vs-safe convention as the rest
     * of this app's color language. */
    g_confirm_label = lv_label_create(g_settings_screen);
    lv_label_set_text_static(g_confirm_label, "Weet je het zeker?");
    lv_obj_set_pos(g_confirm_label, UI_SX(10), UI_SY(110));
    lv_obj_set_style_text_font(g_confirm_label, &lv_font_signage_14, 0);

    g_confirm_yes_btn = lv_button_create(g_settings_screen);
    lv_obj_set_size(g_confirm_yes_btn, UI_SX(200), UI_SY(36));
    lv_obj_set_pos(g_confirm_yes_btn, UI_SX(10), UI_SY(140));
    lv_obj_set_style_bg_color(g_confirm_yes_btn, bgr_fix(lv_color_make(255, 0, 0)), 0);
    lv_obj_set_style_bg_opa(g_confirm_yes_btn, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(g_confirm_yes_btn, confirm_yes_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * confirm_yes_label = lv_label_create(g_confirm_yes_btn);
    lv_label_set_text_static(confirm_yes_label, "Ja, alles wissen");
    lv_obj_set_style_text_font(confirm_yes_label, &lv_font_signage_14, 0);
    lv_obj_set_style_text_color(confirm_yes_label, lv_color_white(), 0);
    lv_obj_center(confirm_yes_label);

    g_confirm_no_btn = lv_button_create(g_settings_screen);
    lv_obj_set_size(g_confirm_no_btn, UI_SX(200), UI_SY(36));
    lv_obj_set_pos(g_confirm_no_btn, UI_SX(220), UI_SY(140));
    lv_obj_set_style_bg_color(g_confirm_no_btn, bgr_fix(lv_color_make(0, 100, 0)), 0);
    lv_obj_set_style_bg_opa(g_confirm_no_btn, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(g_confirm_no_btn, cancel_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * confirm_no_label = lv_label_create(g_confirm_no_btn);
    lv_label_set_text_static(confirm_no_label, "Annuleren");
    lv_obj_set_style_text_font(confirm_no_label, &lv_font_signage_14, 0);
    lv_obj_set_style_text_color(confirm_no_label, lv_color_white(), 0);
    lv_obj_center(confirm_no_label);

    show_confirm_state(false);

    g_status_label = lv_label_create(g_settings_screen);
    lv_obj_set_pos(g_status_label, UI_SX(10), UI_SY(190));
    lv_obj_set_width(g_status_label, UI_SX(460));
    lv_label_set_long_mode(g_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(g_status_label, &lv_font_signage_14, 0);
}

void screen_savings_settings_open(void)
{
    lv_label_set_text(g_status_label, "");
    show_confirm_state(false);
    lv_screen_load(g_settings_screen);
}
