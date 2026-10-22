/* Centrale Instellingen-pagina: het tandwiel in de header opent altijd deze
 * pagina, ongeacht het actieve tabblad - zelfde indeling als het
 * Instellingen-tabblad van de website (Zon, Batterij, Gas, Besparing). Elke
 * knop opent het bestaande instellingenscherm van dat onderdeel; "terug"
 * vanaf dat scherm komt weer hier uit. */
#pragma once

#include <lvgl.h>

/* Bouwt de pagina; `dashboard_screen` is waar de terugknop naartoe gaat.
 * Geeft het scherm terug, zodat de onderliggende instellingenschermen daar
 * naar terugkeren. */
lv_obj_t * screen_settings_menu_init(lv_obj_t * dashboard_screen);

void screen_settings_menu_open(void);
