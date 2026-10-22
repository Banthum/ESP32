// Board-laag CrowPanel Advance 4.3" V1.0: paneel, backlight, GT911-touch en
// LVGL 9 (display + indev + eigen render-taak). De rest van de EMS-code praat
// alleen met LVGL en deze header, nooit met LovyanGFX/de I/O-expander zelf.
#pragma once

#include <lvgl.h>

#define BOARD_LCD_H_RES 800
#define BOARD_LCD_V_RES 480

// Initialiseert I2C, I/O-expander (backlight aan + GT911-reset), paneel, LVGL
// en start de LVGL-taak (core 1). Na terugkeer mag LVGL gebruikt worden, mits
// tussen board_lvgl_lock()/board_lvgl_unlock() vanaf andere taken.
void board_init(void);

// Recursieve mutex rond alle LVGL-aanroepen buiten de LVGL-taak zelf
// (lv_timer-callbacks draaien al binnen de lock).
void board_lvgl_lock(void);
void board_lvgl_unlock(void);

lv_display_t * board_display(void);

// De RGB565-framebuffer (800x480, PSRAM) zoals het paneel hem toont - voor
// /api/screenshot. Kan nullptr zijn als het paneel niet startte.
const uint16_t * board_framebuffer(void);

