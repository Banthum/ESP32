#include "screen_settings_menu.h"

#include "screen_common.h"
#include "screen_settings.h"
#include "screen_battery_settings.h"
#include "screen_price_settings.h"
#include "screen_savings_settings.h"
#include "fonts_signage.h"

static lv_obj_t * g_menu_screen;
static lv_obj_t * g_dashboard_screen;

static void back_btn_event_cb(lv_event_t * e)
{
    (void) e;
    lv_screen_load(g_dashboard_screen);
}

static void open_cb(lv_event_t * e)
{
    void (*open)(void) = (void (*)(void)) lv_event_get_user_data(e);
    open();
}

/* Eén rij: knop met de naam van het onderdeel, uitleg ernaast. */
static void add_entry(int32_t y, const char * name, const char * explain, void (*open)(void))
{
    lv_obj_t * btn = lv_button_create(g_menu_screen);
    lv_obj_set_size(btn, UI_SX(130), UI_SY(46));
    lv_obj_set_pos(btn, UI_SX(10), y);
    lv_obj_set_style_bg_color(btn, bgr_fix(lv_color_make(0, 100, 0)), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(btn, open_cb, LV_EVENT_CLICKED, (void *) open);
    lv_obj_t * label = lv_label_create(btn);
    lv_label_set_text_static(label, name);
    lv_obj_set_style_text_font(label, &lv_font_signage_18, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);

    lv_obj_t * text = lv_label_create(g_menu_screen);
    lv_label_set_text_static(text, explain);
    lv_obj_set_width(text, UI_SX(320));
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(text, &lv_font_signage_14, 0);
    lv_obj_align_to(text, btn, LV_ALIGN_OUT_RIGHT_MID, UI_SX(12), 0);
}

lv_obj_t * screen_settings_menu_init(lv_obj_t * dashboard_screen)
{
    g_dashboard_screen = dashboard_screen;

    g_menu_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_menu_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_menu_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(g_menu_screen, lv_color_white(), 0);
    lv_obj_set_scrollable(g_menu_screen, false);

    lv_obj_t * title = lv_label_create(g_menu_screen);
    lv_label_set_text_static(title, "Instellingen");
    lv_obj_set_pos(title, UI_SX(60), UI_SY(6));
    lv_obj_set_style_text_font(title, &lv_font_signage_18, 0);

    /* Zelfde terugknop als de andere instellingenschermen. */
    lv_obj_t * back_btn = lv_button_create(g_menu_screen);
    lv_obj_set_size(back_btn, UI_SX(44), UI_SY(28));
    lv_obj_set_pos(back_btn, UI_SX(4), UI_SY(2));
    lv_obj_set_style_bg_color(back_btn, bgr_fix(lv_color_make(0, 100, 0)), 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(back_btn, back_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_label = lv_label_create(back_btn);
    lv_label_set_text_static(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
    lv_obj_center(back_label);

    /* Zelfde volgorde als het Instellingen-tabblad van de website. */
    add_entry(UI_SY(44), "Zon", "Adres, panelen en dakorientatie voor de zonverwachting.", screen_settings_open);
    add_entry(UI_SY(110), "Batterij", "Capaciteit, vermogen, SOC-grenzen en aansturing aan/uit.",
              screen_battery_settings_open);
    add_entry(UI_SY(176), "Gas", "Gasprijs tonen en in welke eenheid.", screen_price_settings_open);
    add_entry(UI_SY(242), "Besparing", "Besparingstellers terugzetten naar nul.", screen_savings_settings_open);

    return g_menu_screen;
}

void screen_settings_menu_open(void)
{
    lv_screen_load(g_menu_screen);
}
