// EMSmachine op CrowPanel Advance 4.3" V1.0 - fase 1: board-laag + LVGL 9.
// Tijdelijke testapp: scherm, touch, geheugen en LittleFS controleren.
#include <Arduino.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>

#include "board/board.h"

static lv_obj_t * s_info_label;
static lv_obj_t * s_touch_label;

static void info_timer_cb(lv_timer_t * t)
{
    (void) t;
    lv_label_set_text_fmt(s_info_label,
                          "Intern RAM vrij: %u KB (grootste blok %u KB)\n"
                          "PSRAM vrij: %u KB\n"
                          "LittleFS: %u / %u KB gebruikt",
                          (unsigned) (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                          (unsigned) (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                          (unsigned) (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                          (unsigned) (LittleFS.usedBytes() / 1024), (unsigned) (LittleFS.totalBytes() / 1024));
}

static void screen_touch_cb(lv_event_t * e)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_label_set_text_fmt(s_touch_label, "Touch: x=%d y=%d", (int) p.x, (int) p.y);
    (void) e;
}

static void button_cb(lv_event_t * e)
{
    lv_obj_t * btn = (lv_obj_t *) lv_event_get_target(e);
    static bool on = false;
    on = !on;
    lv_obj_set_style_bg_color(btn, on ? lv_color_make(0, 150, 0) : lv_color_make(150, 0, 0), 0);
}

static void build_test_screen(void)
{
    lv_obj_t * scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, screen_touch_cb, LV_EVENT_PRESSING, nullptr);

    lv_obj_t * title = lv_label_create(scr);
    lv_label_set_text(title, "EMSmachine - fase 1 (LVGL " LVGL_VERSION_INFO ")");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    s_info_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_info_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_info_label, &lv_font_montserrat_20, 0);
    lv_obj_align(s_info_label, LV_ALIGN_LEFT_MID, 30, -20);

    s_touch_label = lv_label_create(scr);
    lv_label_set_text(s_touch_label, "Touch: raak het scherm aan");
    lv_obj_set_style_text_color(s_touch_label, lv_color_make(255, 200, 0), 0);
    lv_obj_set_style_text_font(s_touch_label, &lv_font_montserrat_24, 0);
    lv_obj_align(s_touch_label, LV_ALIGN_BOTTOM_LEFT, 30, -30);

    lv_obj_t * btn = lv_button_create(scr);
    lv_obj_set_size(btn, 200, 80);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -40, 0);
    lv_obj_set_style_bg_color(btn, lv_color_make(150, 0, 0), 0);
    lv_obj_add_event_cb(btn, button_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t * btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Tik mij");
    lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_24, 0);
    lv_obj_center(btn_label);

    // Kleurbalken: rood/groen/blauw los, om RGB-volgorde op dit paneel te bevestigen
    const lv_color_t colors[] = {lv_color_make(255, 0, 0), lv_color_make(0, 255, 0), lv_color_make(0, 0, 255)};
    const char * names[] = {"ROOD", "GROEN", "BLAUW"};
    for(int i = 0; i < 3; i++) {
        lv_obj_t * bar = lv_obj_create(scr);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 120, 50);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar, colors[i], 0);
        lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 30 + i * 140, 80);
        lv_obj_t * l = lv_label_create(bar);
        lv_label_set_text(l, names[i]);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_center(l);
    }

    lv_timer_create(info_timer_cb, 1000, nullptr);
    info_timer_cb(nullptr);
}

void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println("\nEMSmachine ESP32 - fase 1");

    if(!LittleFS.begin(true)) {
        Serial.println("LittleFS mount mislukt");
    }
    Serial.printf("LittleFS: %u/%u bytes\n", (unsigned) LittleFS.usedBytes(), (unsigned) LittleFS.totalBytes());

    board_init();

    board_lvgl_lock();
    build_test_screen();
    board_lvgl_unlock();

    Serial.printf("Intern RAM vrij: %u, PSRAM vrij: %u\n", (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    Serial.println("setup klaar");
}

void loop()
{
    vTaskDelete(nullptr);
}
