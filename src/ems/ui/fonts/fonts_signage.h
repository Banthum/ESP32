#ifndef FONTS_SIGNAGE_H
#define FONTS_SIGNAGE_H

#include <lvgl.h>

/* Oswald SemiBold (Google Fonts, OFL-1.1) at the three text sizes this app
 * uses - a condensed grotesque in the same historic gothic/signage-lettering
 * lineage as DIN 1451, but with real stroke weight (the only free DIN 1451
 * implementation available, OSP-DIN, turned out to be a single Thin/weight-100
 * cut - illegible at small UI sizes on this low-DPI SPI panel regardless of
 * point size, live-confirmed on the real screen). Each size carries a
 * `--lv-fallback` to the matching lv_font_montserrat_* (see
 * fonts/lv_font_signage_*.c's generation comment) so LV_SYMBOL_* glyphs
 * (icons baked into private-use codepoints, outside Oswald's 0x20-0x7E range)
 * still render via Montserrat automatically - no per-widget icon/text split
 * needed, including on composite widgets like lv_keyboard/lv_dropdown that
 * mix both in one font. */
extern const lv_font_t lv_font_signage_10;
extern const lv_font_t lv_font_signage_14;
extern const lv_font_t lv_font_signage_18;

#endif
