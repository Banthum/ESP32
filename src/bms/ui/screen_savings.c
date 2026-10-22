#include "screen_savings.h"

#include <stdio.h>

#include "screen_common.h"
#include "fonts_signage.h"

#define N_PERIODS 5

static const char * const PERIOD_LABELS[N_PERIODS] = {"Vandaag", "Deze week", "Deze maand", "Dit jaar", "Altijd"};

/* ESP32 (800x480): rijen x1,5 hoog, kolommen over de volle breedte (x5/3). */
#define ROW_Y_START UI_SY(50)
#define ROW_HEIGHT UI_SY(36)
#define COL_LABEL_X UI_SX(10)
#define COL_BATTERIJ_X UI_SX(150)
#define COL_BMS_X UI_SX(290)
#define COL_TOTAAL_X UI_SX(400)

static lv_obj_t * g_batterij_labels[N_PERIODS];
static lv_obj_t * g_ems_labels[N_PERIODS];
static lv_obj_t * g_totaal_labels[N_PERIODS];
static lv_obj_t * g_empty_label;
static lv_obj_t * g_grid_root; /* parent of every row/column label above, hidden together as one group */

static void set_eur_label(lv_obj_t * label, double value)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%.2f EUR", value);
    lv_label_set_text(label, buf);
}

void screen_savings_build(lv_obj_t * parent)
{
    lv_obj_t * title = lv_label_create(parent);
    lv_label_set_text_static(title, "Besparing");
    lv_obj_set_pos(title, UI_SX(10), UI_SY(4));
    lv_obj_set_style_text_font(title, &lv_font_signage_18, 0);

    /* No explanatory subtitle here (removed 2026-10-03, Jeroen: "is sws
     * niet te lezen op het EMS" - too small to read on the physical
     * touchscreen at this resolution). The fuller explanation stays on
     * dashboard-web (plenty of room there, see index.html's
     * .savings-explainer) and in dashboard/savings_tracker.py's module
     * docstring. */
    g_grid_root = lv_obj_create(parent);
    lv_obj_remove_style_all(g_grid_root);
    lv_obj_set_pos(g_grid_root, 0, 0);
    lv_obj_set_size(g_grid_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollable(g_grid_root, false);

    /* Column headers - own row above the period rows, same x's as the
     * value columns below so numbers line up under their header. */
    lv_obj_t * h_batterij = lv_label_create(g_grid_root);
    lv_label_set_text_static(h_batterij, "Batterij");
    lv_obj_set_pos(h_batterij, COL_BATTERIJ_X, ROW_Y_START - UI_S(22));
    lv_obj_set_style_text_font(h_batterij, &lv_font_signage_14, 0);

    lv_obj_t * h_ems = lv_label_create(g_grid_root);
    lv_label_set_text_static(h_ems, "BMS");
    lv_obj_set_pos(h_ems, COL_BMS_X, ROW_Y_START - UI_S(22));
    lv_obj_set_style_text_font(h_ems, &lv_font_signage_14, 0);

    lv_obj_t * h_totaal = lv_label_create(g_grid_root);
    lv_label_set_text_static(h_totaal, "Totaal");
    lv_obj_set_pos(h_totaal, COL_TOTAAL_X, ROW_Y_START - UI_S(22));
    lv_obj_set_style_text_font(h_totaal, &lv_font_signage_14, 0);

    for(int i = 0; i < N_PERIODS; i++) {
        int32_t y = ROW_Y_START + i * ROW_HEIGHT;

        lv_obj_t * period_label = lv_label_create(g_grid_root);
        lv_label_set_text_static(period_label, PERIOD_LABELS[i]);
        lv_obj_set_pos(period_label, COL_LABEL_X, y);
        lv_obj_set_style_text_font(period_label, &lv_font_signage_14, 0);

        g_batterij_labels[i] = lv_label_create(g_grid_root);
        lv_label_set_text_static(g_batterij_labels[i], "-");
        lv_obj_set_pos(g_batterij_labels[i], COL_BATTERIJ_X, y);
        lv_obj_set_style_text_font(g_batterij_labels[i], &lv_font_signage_14, 0);

        g_ems_labels[i] = lv_label_create(g_grid_root);
        lv_label_set_text_static(g_ems_labels[i], "-");
        lv_obj_set_pos(g_ems_labels[i], COL_BMS_X, y);
        lv_obj_set_style_text_font(g_ems_labels[i], &lv_font_signage_14, 0);

        g_totaal_labels[i] = lv_label_create(g_grid_root);
        lv_label_set_text_static(g_totaal_labels[i], "-");
        lv_obj_set_pos(g_totaal_labels[i], COL_TOTAAL_X, y);
        lv_obj_set_style_text_font(g_totaal_labels[i], &lv_font_signage_14, 0);
    }

    g_empty_label = screen_common_add_empty_state_label(
        parent, g_grid_root, "Nog geen besparingsdata\n(begint te tellen zodra de batterij actief is)");
    lv_obj_set_style_text_align(g_empty_label, LV_TEXT_ALIGN_CENTER, 0);
}

void screen_savings_update(const dashboard_state_t * state)
{
    screen_common_set_hidden(g_empty_label, state->savings_valid);
    screen_common_set_hidden(g_grid_root, !state->savings_valid);
    if(!state->savings_valid) return;

    const double batterij[N_PERIODS] = {
        state->savings_dag_batterij_eur, state->savings_week_batterij_eur, state->savings_maand_batterij_eur,
        state->savings_jaar_batterij_eur, state->savings_altijd_batterij_eur,
    };
    const double ems[N_PERIODS] = {
        state->savings_dag_ems_eur, state->savings_week_ems_eur, state->savings_maand_ems_eur,
        state->savings_jaar_ems_eur, state->savings_altijd_ems_eur,
    };

    for(int i = 0; i < N_PERIODS; i++) {
        set_eur_label(g_batterij_labels[i], batterij[i]);
        set_eur_label(g_ems_labels[i], ems[i]);
        set_eur_label(g_totaal_labels[i], batterij[i] + ems[i]);
    }
}
