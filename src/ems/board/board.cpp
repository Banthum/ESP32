#include "board.h"

#include <Arduino.h>
#include <TCA9534.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "lgfx_crowpanel_v10.h"

// I/O-expander op 0x18 (PCA9557 op de PCB, registercompatibel met TCA9534).
// Pin 1 = backlight, pin 2 = GT911-reset - zie Elecrow V1.0 lesson-03 setup().
static constexpr uint8_t IOEX_ADDR = 0x18;
static constexpr uint8_t IOEX_PIN_BACKLIGHT = 1;
static constexpr uint8_t IOEX_PIN_TOUCH_RST = 2;
// GT911 INT-pin: laag tijdens reset selecteert I2C-adres 0x5D.
static constexpr int GT911_INT_PIN = 1;

// LVGL tekent in 2 kleine buffers in intern RAM (zoals Elecrow's V1.1-V1.3-
// code), niet in PSRAM: de PSRAM voedt al de RGB-framebuffer, en extra PSRAM-
// verkeer gaf flikkering (zie src/elecrow_lesson03). 20 i.p.v. Elecrow's 40
// regels: mbedTLS (HTTPS naar prijs-/zon-API's) alloceert ~35 KB+ uit intern
// RAM per verbinding, en met 2x40 regels bleef er na WiFi maar 87 KB over.
static constexpr uint32_t LVGL_BUFFER_LINES = 20;

static constexpr uint32_t LVGL_TASK_STACK = 8 * 1024;
static constexpr UBaseType_t LVGL_TASK_PRIO = 2;
static constexpr BaseType_t LVGL_TASK_CORE = 1;

static LGFX_CrowPanelV10 s_gfx;
static TCA9534 s_ioex;
static lv_display_t * s_disp = nullptr;
static SemaphoreHandle_t s_lvgl_mutex = nullptr;

static void ioex_and_touch_reset(void)
{
    s_ioex.attach(Wire);
    s_ioex.setDeviceAddress(IOEX_ADDR);
    for(uint8_t pin = 1; pin <= 4; pin++) {
        s_ioex.config(pin, TCA9534::Config::OUT);
    }
    s_ioex.output(IOEX_PIN_BACKLIGHT, TCA9534::Level::H);

    pinMode(GT911_INT_PIN, OUTPUT);
    digitalWrite(GT911_INT_PIN, LOW);
    s_ioex.output(IOEX_PIN_TOUCH_RST, TCA9534::Level::L);
    delay(20);
    s_ioex.output(IOEX_PIN_TOUCH_RST, TCA9534::Level::H);
    delay(100);
    pinMode(GT911_INT_PIN, INPUT);
}

static void flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map)
{
    if(s_gfx.getStartCount() > 0) {
        s_gfx.endWrite();
    }
    s_gfx.pushImageDMA(area->x1, area->y1, lv_area_get_width(area), lv_area_get_height(area),
                       (lgfx::rgb565_t *) px_map);
    lv_display_flush_ready(disp);
}

static void touch_read_cb(lv_indev_t * indev, lv_indev_data_t * data)
{
    (void) indev;
    uint16_t x, y;
    if(s_gfx.getTouch(&x, &y)) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static uint32_t tick_cb(void)
{
    return millis();
}

static void lvgl_task(void * arg)
{
    (void) arg;
    for(;;) {
        board_lvgl_lock();
        uint32_t wait_ms = lv_timer_handler();
        board_lvgl_unlock();
        if(wait_ms == LV_NO_TIMER_READY || wait_ms > LV_DEF_REFR_PERIOD) {
            wait_ms = LV_DEF_REFR_PERIOD;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms < 1 ? 1 : wait_ms));
    }
}

void board_lvgl_lock(void)
{
    xSemaphoreTakeRecursive(s_lvgl_mutex, portMAX_DELAY);
}

void board_lvgl_unlock(void)
{
    xSemaphoreGiveRecursive(s_lvgl_mutex);
}

lv_display_t * board_display(void)
{
    return s_disp;
}

void board_init(void)
{
    s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();

    Wire.begin(15, 16);
    delay(50);
    ioex_and_touch_reset();

    s_gfx.init();
    s_gfx.initDMA();
    s_gfx.startWrite();
    s_gfx.fillScreen(TFT_BLACK);

    lv_init();
    lv_tick_set_cb(tick_cb);

    s_disp = lv_display_create(BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, flush_cb);

    size_t buffer_size = BOARD_LCD_H_RES * LVGL_BUFFER_LINES * sizeof(uint16_t);
    void * buf1 = heap_caps_malloc(buffer_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    void * buf2 = heap_caps_malloc(buffer_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(!buf1 || !buf2) {
        Serial.println("board: LVGL-buffers in intern RAM mislukt");
    }
    lv_display_set_buffers(s_disp, buf1, buf2, buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t * indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", LVGL_TASK_STACK, nullptr, LVGL_TASK_PRIO, nullptr, LVGL_TASK_CORE);
}
