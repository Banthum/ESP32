#include "screen_energyflow.h"

#include <math.h>
#include <stdio.h>
#include <time.h>

#include "screen_common.h"
#include "fonts_signage.h"

/* Node layout - a fixed cross/diamond (Net left, Home right, Zon top,
 * Batterij bottom). Full mesh, on request: every one of the 4 nodes gets
 * a straight line to every other node (6 lines total - the 4 diamond
 * edges plus the Net-Home and Zon-Batterij cross lines), matching a
 * hand-drawn K4 reference Jeroen provided.
 *
 * **Core invariant: Home can only ever be a sink, never a source** (it
 * generates nothing of its own - see its permanent home_color() ring
 * below). This app has exactly 3 independently-measured signed/unsigned
 * quantities (grid W, battery W, solar forecast W) plus Home as their
 * derived residual (screen_energyflow_update()'s Home block) - so any
 * time Home-Net or Home-Batterij would need to animate a flow *leaving*
 * Home (grid export, or battery charging), that's necessarily wrong: the
 * energy has to actually be coming from Zon and/or Batterij (for an
 * export) or Zon and/or Net (for a battery charge), just routed through
 * the same physical meter point as Home. Live-caught twice by Jeroen:
 * first "1000W zon, 0W battery, 800W export" rendering as Home->Net, then
 * (after that fix) "batterij laadt op, maar de stroom lijkt naar het huis
 * te gaan" - a charging battery still rendering as if Home->Batterij.
 *
 * Fix: Net-Home and Batterij-Home only EVER animate flow *into* Home
 * (import, and discharge respectively) - never the reverse. The other 3
 * mesh edges (Zon-Net, Zon-Batterij, Batterij-Net) carry the *derived*
 * routing for the reverse cases: export goes out via Zon-Net (if Zon is
 * surplus-active) and/or Batterij-Net (if Batterij is discharging while
 * the grid exports - i.e. battery surplus beyond Home's own need);
 * charging comes in via Zon-Batterij (if Zon is active) and/or
 * Batterij-Net the other direction (Net->Batterij, if the grid is
 * importing while the battery charges). This is a *routing* decision
 * from booleans/thresholds, not a measured split - there's no sensor for
 * "how much of this export/charge came from Zon vs Net vs Batterij" - so
 * multiple of these derived edges can legitimately be active at once
 * (e.g. Zon simultaneously covering Home, exporting surplus, AND
 * charging the battery). See screen_energyflow_update()'s dedicated
 * "Derived routing edges" section for the actual logic. Icons need
 * lv_font_montserrat_20 (already used for the header's hamburger/gear
 * icons) - the custom "signage" fonts only cover 0x20-0x7E (see
 * fonts_signage.h's generation comment), not the symbol codepoints
 * LV_SYMBOL_* map to. */
/* Tab-content-hoogte is 276px (320 - HEADER_HEIGHT, zie dashboard_ui.c),
 * NIET het volledige 320px-scherm - alle Y-waarden hieronder zijn lokaal
 * aan die 276px. ZON_Y/BATT_Y staan allebei 58px van NET_Y/HOME_Y=151 -
 * symmetrisch (2026-09-30, live-gemeld op het fysieke scherm: de kruising
 * zat merkbaar niet in het midden - eerst gelijkgetrokken op 55px elk,
 * daarna nog 3px verder uit elkaar getrokken op verzoek ("langere
 * verticale lijn")). 58px is zo dicht mogelijk tegen de fysieke grens aan
 * zonder het batterij-naam-label te laten clippen: bottom-edge-check
 * BATT_Y+NODE_R+~18 (label-hoogte) = 209+44+18=271, nog 5px marge onder
 * de 276px-grens - de website-versie (dashboard-web/static/app.js) heeft
 * deze fysieke-scherm-beperking niet en staat daar los van veel verder
 * uit elkaar (90px). Ga hier niet verder dan 58 zonder eerst een echte
 * framebuffer-dump te controleren op clipping. */
/* ESP32 (800x480): posities per as geschaald (UI_SX/UI_SY), de cirkels
 * uniform (UI_S) zodat ze rond blijven. Tab-hoogte is hier 414px
 * (480 - UI_SY(44)); de bottom-edge-check van hierboven wordt
 * UI_SY(209)+UI_S(44)+~33 (label) = 412, binnen die 414. */
#define NODE_R UI_S(44)
#define NET_X UI_SX(80)
#define NET_Y UI_SY(151)
#define HOME_X UI_SX(400)
#define HOME_Y UI_SY(151)
#define ZON_X UI_SX(240)
#define ZON_Y UI_SY(93)
#define BATT_X UI_SX(240)
#define BATT_Y UI_SY(209)

#define CURVE_POINTS 20
#define DOT_R UI_S(5)
#define DOT_ANIM_DURATION_MS 1500

/* Deadband (W) before a flow is considered "active" - avoids the dot/line
 * flickering on/off from measurement noise around exactly 0, same idea as
 * marstek_client.py's IDLE_DEADBAND_W. */
#define FLOW_DEADBAND_W 15.0

typedef struct {
    lv_obj_t * circle;
    lv_obj_t * icon;
    lv_obj_t * value_label;
    lv_obj_t * name_label;
} flow_node_t;

static flow_node_t g_net_node, g_zon_node, g_home_node, g_batt_node;

/* De Zon-node toont de Forecast.Solar-verwachting voor het huidige uur
 * als hoofdwaarde, NIET marstek_client.py's solar_power_w - er bestaat in
 * dit product structureel geen live zonmeting (de panelen zijn nooit
 * fysiek gekoppeld aan de PV-poort van de batterij, precies de reden
 * waarom de Forecast.Solar-integratie er is, zie CLAUDE.md). solar_power_w
 * blijft dus altijd 0/near-0, ongeacht echte zoninstraling - gebruiken
 * zou een permanent misleidende "0 W"-node geven. Home's eigen
 * vermogensbalans (verderop) blijft wél solar_power_w gebruiken - die
 * som klopt toevallig al zonder een aparte zonterm, want de P1-meter
 * meet al ná de echte (elders opgewekte) zon-opbrengst. */

/* All 6 mesh edges now carry real dot/anim machinery - Zon-Net,
 * Zon-Batterij and Batterij-Net used to be purely decorative/static, but
 * all 3 turned out to have plausible derived flows once Home-as-sink-only
 * was enforced properly (see the layout comment above). */
static lv_obj_t * g_zon_line;
static lv_obj_t * g_net_line;
static lv_obj_t * g_batt_line;
static lv_obj_t * g_zon_net_line;
static lv_obj_t * g_zon_batt_line;
static lv_obj_t * g_batt_net_line;
static lv_obj_t * g_zon_dot;
static lv_obj_t * g_net_dot;
static lv_obj_t * g_batt_dot;
static lv_obj_t * g_zon_net_dot;
static lv_obj_t * g_zon_batt_dot;
static lv_obj_t * g_batt_net_dot;
static lv_anim_t g_zon_anim;
static lv_anim_t g_net_anim;
static lv_anim_t g_batt_anim;
static lv_anim_t g_zon_net_anim;
static lv_anim_t g_zon_batt_anim;
static lv_anim_t g_batt_net_anim;

static lv_point_precise_t g_zon_home_points[CURVE_POINTS];
static lv_point_precise_t g_net_home_points[CURVE_POINTS];
static lv_point_precise_t g_batt_home_points[CURVE_POINTS];
static lv_point_precise_t g_zon_net_points[CURVE_POINTS];
static lv_point_precise_t g_zon_batt_points[CURVE_POINTS];
static lv_point_precise_t g_batt_net_points[CURVE_POINTS];

/* -2 is not a real direction (-1/0/+1 are) - forces the first
 * screen_energyflow_update() call to always (re)apply, same NAN-sentinel
 * idea as screen_solar.c's g_axis_applied_for_kwp, just int-flavored since
 * "direction" is inherently discrete here. g_net_dir_applied/
 * g_batt_dir_applied gate their node's OWN border color only (unchanged
 * concept); which edge(s) actually animate is gated by the *_applied
 * vars below instead, since that now depends on combinations of
 * net_dir/zon_active/batt_dir rather than a single node's own sign. */
static int g_zon_dir_applied = -2;
static int g_net_dir_applied = -2;
static int g_batt_dir_applied = -2;
/* 0/1 booleans-as-int (still -2-sentineled for "never applied yet"). */
static int g_net_home_applied = -2;
static int g_batt_home_applied = -2;
static int g_zon_net_applied = -2;
static int g_zon_batt_applied = -2;
/* Batterij-Net is the one derived edge with 2 possible directions
 * (Batterij->Net when battery-discharge-surplus is exported, Net->
 * Batterij when grid import charges the battery) - so this one's a real
 * tri-state (-1/0/1), not just a 0/1 boolean, same -2 "never applied"
 * sentinel idea as the others. */
static int g_batt_net_applied = -2;

/* Colors - all independently checked against the fb_ili9486 scramble-
 * safety rule (green exactly 0/255, or blue exactly 0, or red exactly
 * equal blue - see screen_price.c's cursor comment / CLAUDE.md "Scherm"
 * section), same as every other explicit color in this app. */
static lv_color_t net_color(void) { return bgr_fix(lv_color_make(0, 0, 220)); }     /* blue, green=0 route */
static lv_color_t zon_color(void) { return bgr_fix(lv_color_make(255, 152, 0)); }   /* orange, blue=0 route */
static lv_color_t home_color(void) { return bgr_fix(lv_color_make(0, 255, 200)); }  /* teal, green=255 route */
static lv_color_t batt_color(void) { return bgr_fix(lv_color_make(80, 255, 120)); } /* green, green=255 route */
static lv_color_t dim_color(void) { return bgr_fix(lv_color_make(90, 90, 90)); }    /* grey, red==blue route */

static void compute_quad_bezier(lv_point_precise_t * out, int n, float x0, float y0, float cx, float cy, float x1,
                                 float y1)
{
    for(int i = 0; i < n; i++) {
        float t = (float) i / (n - 1);
        float mt = 1.0f - t;
        out[i].x = mt * mt * x0 + 2 * mt * t * cx + t * t * x1;
        out[i].y = mt * mt * y0 + 2 * mt * t * cy + t * t * y1;
    }
}

/* Moves point (x0,y0) toward (x1,y1) by distance `r`, used to start/end
 * mesh lines at each node's circle edge (along the actual line direction)
 * rather than at its center - needed now that 4 of the 6 mesh lines are
 * genuine diagonals, not just cardinal (horizontal/vertical) offsets. */
static void offset_towards(float x0, float y0, float x1, float y1, float r, float * out_x, float * out_y)
{
    float dx = x1 - x0;
    float dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    *out_x = x0 + dx / len * r;
    *out_y = y0 + dy / len * r;
}

static lv_obj_t * create_node_circle(lv_obj_t * parent, int32_t cx, int32_t cy)
{
    lv_obj_t * circle = lv_obj_create(parent);
    lv_obj_remove_style_all(circle); /* strip the default theme card style, see dashboard_ui.c's header for why */
    lv_obj_set_size(circle, NODE_R * 2, NODE_R * 2);
    lv_obj_set_pos(circle, cx - NODE_R, cy - NODE_R);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circle, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(circle, UI_S(3), 0);
    lv_obj_set_scrollable(circle, false);
    return circle;
}

static void create_node(lv_obj_t * parent, flow_node_t * node, int32_t cx, int32_t cy, const char * icon,
                         const char * name)
{
    node->circle = create_node_circle(parent, cx, cy);

    node->icon = lv_label_create(node->circle);
    lv_label_set_text_static(node->icon, icon);
    lv_obj_set_style_text_font(node->icon, &lv_font_montserrat_30, 0);
    lv_obj_set_style_text_color(node->icon, lv_color_white(), 0);
    lv_obj_align(node->icon, LV_ALIGN_TOP_MID, 0, UI_S(10));

    /* signage_18 (i.p.v. _14) voor betere leesbaarheid nu de node-cirkels
     * groter zijn (NODE_R 34->44, op verzoek). Center-aligned zodat Net/
     * Batterij's twee pijltje-regels (zie screen_energyflow_update()) als
     * nette blok in het midden staan i.p.v. links uitgelijnd. */
    node->value_label = lv_label_create(node->circle);
    lv_label_set_text_static(node->value_label, "-");
    lv_obj_set_style_text_font(node->value_label, &lv_font_signage_18, 0);
    lv_obj_set_style_text_color(node->value_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(node->value_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(node->value_label, LV_ALIGN_BOTTOM_MID, 0, -UI_S(8));

    node->name_label = lv_label_create(parent);
    lv_label_set_text_static(node->name_label, name);
    lv_obj_set_style_text_font(node->name_label, &lv_font_signage_14, 0);
    lv_obj_set_style_text_color(node->name_label, lv_color_white(), 0);
    lv_obj_align_to(node->name_label, node->circle, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_S(4));
}

static lv_obj_t * create_flow_line(lv_obj_t * parent, lv_point_precise_t * points, int n)
{
    lv_obj_t * line = lv_line_create(parent);
    lv_line_set_points(line, points, n);
    lv_obj_set_style_line_width(line, UI_S(3), 0);
    lv_obj_set_style_line_color(line, dim_color(), 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    return line;
}

static lv_obj_t * create_dot(lv_obj_t * parent)
{
    lv_obj_t * dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, DOT_R * 2, DOT_R * 2);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(dot, false);
    lv_obj_set_hidden(dot, true);
    return dot;
}

static void zon_dot_exec_cb(void * var, int32_t v)
{
    lv_point_precise_t p = g_zon_home_points[v];
    lv_obj_set_pos((lv_obj_t *) var, (int32_t) p.x - DOT_R, (int32_t) p.y - DOT_R);
}

static void net_dot_exec_cb(void * var, int32_t v)
{
    lv_point_precise_t p = g_net_home_points[v];
    lv_obj_set_pos((lv_obj_t *) var, (int32_t) p.x - DOT_R, (int32_t) p.y - DOT_R);
}

static void batt_dot_exec_cb(void * var, int32_t v)
{
    lv_point_precise_t p = g_batt_home_points[v];
    lv_obj_set_pos((lv_obj_t *) var, (int32_t) p.x - DOT_R, (int32_t) p.y - DOT_R);
}

static void zon_net_dot_exec_cb(void * var, int32_t v)
{
    lv_point_precise_t p = g_zon_net_points[v];
    lv_obj_set_pos((lv_obj_t *) var, (int32_t) p.x - DOT_R, (int32_t) p.y - DOT_R);
}

static void zon_batt_dot_exec_cb(void * var, int32_t v)
{
    lv_point_precise_t p = g_zon_batt_points[v];
    lv_obj_set_pos((lv_obj_t *) var, (int32_t) p.x - DOT_R, (int32_t) p.y - DOT_R);
}

static void batt_net_dot_exec_cb(void * var, int32_t v)
{
    lv_point_precise_t p = g_batt_net_points[v];
    lv_obj_set_pos((lv_obj_t *) var, (int32_t) p.x - DOT_R, (int32_t) p.y - DOT_R);
}

/* Starts (or restarts, if already running) `dot` looping along its
 * CURVE_POINTS-length path, forward (0..N-1) or backward (N-1..0)
 * depending on `forward` - direction encodes the flow's current
 * direction (e.g. discharging vs. charging). Only called when the
 * direction actually *changes* (see screen_energyflow_update()), not
 * every poll - restarting a still-correct animation would visibly jump
 * the dot back to the start every cycle. */
static void set_flow_dot(lv_anim_t * a, lv_obj_t * dot, lv_anim_exec_xcb_t exec_cb, bool forward)
{
    lv_anim_delete(dot, NULL);
    lv_anim_init(a);
    lv_anim_set_var(a, dot);
    lv_anim_set_exec_cb(a, exec_cb);
    lv_anim_set_duration(a, DOT_ANIM_DURATION_MS);
    lv_anim_set_repeat_count(a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(a, lv_anim_path_linear);
    if(forward) lv_anim_set_values(a, 0, CURVE_POINTS - 1);
    else lv_anim_set_values(a, CURVE_POINTS - 1, 0);
    lv_obj_set_hidden(dot, false);
    lv_anim_start(a);
}

static void clear_flow_dot(lv_obj_t * dot)
{
    lv_anim_delete(dot, NULL);
    lv_obj_set_hidden(dot, true);
}

void screen_energyflow_build(lv_obj_t * parent)
{
    lv_obj_t * title = lv_label_create(parent);
    lv_label_set_text_static(title, "Energiestromen");
    lv_obj_set_pos(title, UI_SX(10), UI_SY(4));
    lv_obj_set_style_text_font(title, &lv_font_signage_18, 0);

    /* Zon-Home, Batterij-Home and Net-Home are straight lines from circle
     * edge to circle edge (offset_towards() finds that edge point along
     * each line's real direction - Zon-Home/Batterij-Home are genuine
     * diagonals, only Net-Home is cardinal/horizontal). Still built as
     * CURVE_POINTS-length arrays (via the degenerate-bezier trick, control
     * point = midpoint) rather than plain 2-point lines, purely so the
     * existing dot-animation machinery (zon_dot_exec_cb() etc., which
     * indexes these arrays 0..CURVE_POINTS-1) keeps working unchanged. */
    float zx0, zy0, zx1, zy1, bx0, by0, bx1, by1;
    offset_towards(ZON_X, ZON_Y, HOME_X, HOME_Y, NODE_R, &zx0, &zy0);
    offset_towards(HOME_X, HOME_Y, ZON_X, ZON_Y, NODE_R, &zx1, &zy1);
    compute_quad_bezier(g_zon_home_points, CURVE_POINTS, zx0, zy0, (zx0 + zx1) / 2.0f, (zy0 + zy1) / 2.0f, zx1, zy1);

    offset_towards(BATT_X, BATT_Y, HOME_X, HOME_Y, NODE_R, &bx0, &by0);
    offset_towards(HOME_X, HOME_Y, BATT_X, BATT_Y, NODE_R, &bx1, &by1);
    compute_quad_bezier(g_batt_home_points, CURVE_POINTS, bx0, by0, (bx0 + bx1) / 2.0f, (by0 + by1) / 2.0f, bx1, by1);

    compute_quad_bezier(g_net_home_points, CURVE_POINTS, NET_X + NODE_R, NET_Y, (NET_X + NODE_R + HOME_X - NODE_R) / 2.0f,
                         NET_Y, HOME_X - NODE_R, HOME_Y);

    /* Zon-Net, Zon-Batterij and Batterij-Net: same degenerate-straight-
     * line/CURVE_POINTS trick as the 3 edges above - all 3 need real dot
     * animations now (see the layout comment above for what each one
     * represents). Point-array direction convention (used by
     * screen_energyflow_update()'s routing logic below): index 0 is
     * always the *first*-named node in the pair, index CURVE_POINTS-1 the
     * second - so e.g. for Batterij-Net, index 0 is the Batterij-side
     * point and the last index is the Net-side point. offset_towards()
     * takes float* out params - go through local floats rather than
     * &point.x directly, since lv_value_precise_t is only sometimes
     * literally `float` (see lv_types.h, depends on LV_USE_FLOAT) and
     * &point.x could otherwise be the wrong pointer type. */
    float ex0, ey0, ex1, ey1;
    offset_towards(ZON_X, ZON_Y, NET_X, NET_Y, NODE_R, &ex0, &ey0);
    offset_towards(NET_X, NET_Y, ZON_X, ZON_Y, NODE_R, &ex1, &ey1);
    compute_quad_bezier(g_zon_net_points, CURVE_POINTS, ex0, ey0, (ex0 + ex1) / 2.0f, (ey0 + ey1) / 2.0f, ex1, ey1);

    offset_towards(ZON_X, ZON_Y, BATT_X, BATT_Y, NODE_R, &ex0, &ey0);
    offset_towards(BATT_X, BATT_Y, ZON_X, ZON_Y, NODE_R, &ex1, &ey1);
    compute_quad_bezier(g_zon_batt_points, CURVE_POINTS, ex0, ey0, (ex0 + ex1) / 2.0f, (ey0 + ey1) / 2.0f, ex1, ey1);

    offset_towards(BATT_X, BATT_Y, NET_X, NET_Y, NODE_R, &ex0, &ey0);
    offset_towards(NET_X, NET_Y, BATT_X, BATT_Y, NODE_R, &ex1, &ey1);
    compute_quad_bezier(g_batt_net_points, CURVE_POINTS, ex0, ey0, (ex0 + ex1) / 2.0f, (ey0 + ey1) / 2.0f, ex1, ey1);

    g_zon_line = create_flow_line(parent, g_zon_home_points, CURVE_POINTS);
    g_batt_line = create_flow_line(parent, g_batt_home_points, CURVE_POINTS);
    g_net_line = create_flow_line(parent, g_net_home_points, CURVE_POINTS);
    g_zon_net_line = create_flow_line(parent, g_zon_net_points, CURVE_POINTS);
    g_zon_batt_line = create_flow_line(parent, g_zon_batt_points, CURVE_POINTS);
    g_batt_net_line = create_flow_line(parent, g_batt_net_points, CURVE_POINTS);

    /* Dots created after the lines so they draw on top of them (LVGL
     * draws children/siblings in creation order). */
    g_zon_dot = create_dot(parent);
    g_net_dot = create_dot(parent);
    g_batt_dot = create_dot(parent);
    g_zon_net_dot = create_dot(parent);
    g_zon_batt_dot = create_dot(parent);
    g_batt_net_dot = create_dot(parent);
    lv_obj_set_style_bg_color(g_zon_dot, zon_color(), 0);
    lv_obj_set_style_bg_color(g_net_dot, net_color(), 0);
    lv_obj_set_style_bg_color(g_batt_dot, batt_color(), 0);
    /* Fixed per-edge identity color, same convention as the 3 "primary"
     * edges above: an edge touching Net uses net_color() (Zon-Net,
     * Batterij-Net), the one derived edge that doesn't touch Net
     * (Zon-Batterij) uses batt_color() instead, matching what it feeds
     * into. Not meant to encode "which node the energy truly originated
     * from" - these are routing edges, not independent measurements, see
     * the layout comment above. */
    lv_obj_set_style_bg_color(g_zon_net_dot, net_color(), 0);
    lv_obj_set_style_bg_color(g_zon_batt_dot, batt_color(), 0);
    lv_obj_set_style_bg_color(g_batt_net_dot, net_color(), 0);

    /* Nodes created last so their circles draw over the line/dot endpoints
     * meeting them, same reasoning as the dots above. */
    create_node(parent, &g_net_node, NET_X, NET_Y, LV_SYMBOL_SHUFFLE, "Net");
    create_node(parent, &g_zon_node, ZON_X, ZON_Y, LV_SYMBOL_CHARGE, "Zon");
    create_node(parent, &g_home_node, HOME_X, HOME_Y, LV_SYMBOL_HOME, "Home");
    create_node(parent, &g_batt_node, BATT_X, BATT_Y, LV_SYMBOL_BATTERY_FULL, "Batterij");
    lv_obj_set_style_border_color(g_net_node.circle, dim_color(), 0);
    lv_obj_set_style_border_color(g_zon_node.circle, dim_color(), 0);
    /* Home's ring stays home_color() permanently, unlike the other 3 -
     * matches the reference screenshot (Home's border is solid teal
     * there too) and makes sense semantically: Home is always a real
     * sink (it's always consuming something), it just has no single
     * "flow direction" of its own the way a source/sink pair does. */
    lv_obj_set_style_border_color(g_home_node.circle, home_color(), 0);
    lv_obj_set_style_border_color(g_batt_node.circle, dim_color(), 0);
}

/* `battery_icon_for_soc()` picks a battery glyph matching the SOC roughly
 * the way a real battery indicator would (full/3-quarters/half/quarter/
 * empty) - purely cosmetic, no functional meaning beyond that. */
static const char * battery_icon_for_soc(int soc_pct)
{
    if(soc_pct >= 80) return LV_SYMBOL_BATTERY_FULL;
    if(soc_pct >= 55) return LV_SYMBOL_BATTERY_3;
    if(soc_pct >= 30) return LV_SYMBOL_BATTERY_2;
    if(soc_pct >= 10) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

void screen_energyflow_update(const dashboard_state_t * state)
{
    char buf[40];

    /* Every branch below skips ALL updates (animation, color, AND value
     * text) when its field is invalid, rather than treating "invalid" as
     * "zero/inactive" - live-confirmed necessary: the Marstek battery's
     * already-documented UDP flakiness (CLAUDE.md - "UDP-requests kort na
     * elkaar kunnen zonder echte netwerkoorzaak timeouten") intermittently
     * drops battery_power_w_valid for a single 10s poll, and treating that
     * as "stopped discharging" was live-observed to restart the flow
     * animation from scratch every time it happened - a completely stuck-
     * looking dot in practice. Same "widgets show the last known state
     * during a gap" principle docs/ipc_protocol.md already documents for
     * the whole app, just applied consistently here too. */

    /* Zon: gedreven door de Forecast.Solar-curve (huidig uur), niet door
     * solar_power_w - zie de comment bij g_net_node/g_zon_node's
     * declaratie hierboven voor waarom er structureel geen live bron is.
     * Altijd >=0 (opwekking), dus "active" is gewoon "boven de deadband"
     * - geen richting te bepalen, de dot loopt alleen ooit Zon->Home.
     * Eenheid: solar_forecast_curve is kWh voor dat uur (zie
     * screen_solar.c), *1000 voor een gemiddeld-W-schatting.
     * `zon_forecast_w`/`zon_forecast_valid`/`zon_active` zijn bewust
     * function-scope (niet lokaal in de if hieronder) - de Home-berekening
     * verderop hergebruikt zon_forecast_w, en het Net-blok hergebruikt
     * zon_active om te bepalen of een export via de Zon-Net-lijn getoond
     * moet worden i.p.v. via Net-Home (zie de layout-comment bovenaan dit
     * bestand). */
    double zon_forecast_w = 0.0;
    bool zon_active = false;
    time_t zon_forecast_now = time(NULL);
    struct tm zon_forecast_tm;
    localtime_r(&zon_forecast_now, &zon_forecast_tm);
    int zon_forecast_hour = zon_forecast_tm.tm_hour;
    int zon_forecast_min = zon_forecast_tm.tm_min;
    bool zon_forecast_valid = zon_forecast_hour >= 0 && zon_forecast_hour < IPC_SOLAR_FORECAST_LEN &&
                               state->solar_forecast_curve_valid[zon_forecast_hour];
    if(zon_forecast_valid) {
        zon_forecast_w = state->solar_forecast_curve[zon_forecast_hour] * 1000.0;
        /* Lineair interpoleren naar het volgende uur, herberekend in
         * 5-minuten-stappen (:00/:05/.../:55) i.p.v. continu meebewegen met
         * elke UI_TIMER_PERIOD_MS-poll (500ms, zie dashboard_ui.c) - de
         * brondata zelf is uur-resolutie (Forecast.Solar), dus continu
         * "vloeiend" meebewegen zou een nauwkeurigheid suggereren die er
         * niet is. Op verzoek: geeft een geleidelijk op-/aflopend getal
         * i.p.v. een harde sprong bij ieder heel uur. Geen volgend-uur-data
         * (laatste uur van de dag, of dat uur nog niet gevuld) -> gewoon de
         * huidige-uur-waarde ongewijzigd, zelfde gedrag als vóór deze
         * interpolatie. */
        int zon_next_hour = zon_forecast_hour + 1;
        if(zon_next_hour < IPC_SOLAR_FORECAST_LEN && state->solar_forecast_curve_valid[zon_next_hour]) {
            double zon_forecast_next_w = state->solar_forecast_curve[zon_next_hour] * 1000.0;
            double zon_frac = (double) ((zon_forecast_min / 5) * 5) / 60.0;
            zon_forecast_w += (zon_forecast_next_w - zon_forecast_w) * zon_frac;
        }
        zon_active = zon_forecast_w > FLOW_DEADBAND_W;
        int zon_dir = zon_active ? 1 : 0;
        if(zon_dir != g_zon_dir_applied) {
            if(zon_active) set_flow_dot(&g_zon_anim, g_zon_dot, zon_dot_exec_cb, true);
            else clear_flow_dot(g_zon_dot);
            lv_obj_set_style_line_color(g_zon_line, zon_active ? zon_color() : dim_color(), 0);
            lv_obj_set_style_border_color(g_zon_node.circle, zon_active ? zon_color() : dim_color(), 0);
            g_zon_dir_applied = zon_dir;
        }
        /* Label text is set below, near the Home-block - it may get
         * corrected for the dynamic min_home_w floor there, not just the
         * raw forecast value (see that block's comment). */
    }
    else if(g_zon_dir_applied != 0) {
        /* No forecast entry for this hour at all (e.g. every night after
         * sunset, see solar_forecast_client.py - Forecast.Solar simply
         * stops reporting, this isn't a transient UDP-style glitch like
         * the "skip instead of blanking" reasoning above is meant for).
         * Live-caught 2026-10-11: without this branch the node just kept
         * showing whatever daylight wattage was last valid (e.g. 537 W)
         * all night, with the dot still animating orange, while
         * screen_solar.c's own forecast correctly showed 0 for the same
         * hour - reset to dim/inactive and "-" instead of freezing. */
        clear_flow_dot(g_zon_dot);
        lv_obj_set_style_line_color(g_zon_line, dim_color(), 0);
        lv_obj_set_style_border_color(g_zon_node.circle, dim_color(), 0);
        g_zon_dir_applied = 0;
        lv_label_set_text_static(g_zon_node.value_label, "-");
    }

    /* net_dir/batt_dir zijn bewust function-scope (net als zon_active
     * hierboven) - de "Derived routing edges"-sectie verderop combineert
     * alle drie om te bepalen welke van de 3 afgeleide randen (Zon-Net,
     * Zon-Batterij, Batterij-Net) een export/laadstroom mogen tonen.
     * batt_dir blijft 0 (inactief) zolang battery_power_w_valid false is -
     * veilig default, betekent alleen dat routing geen stroom aan een
     * batterijstatus toeschrijft die niet betrouwbaar bekend is. */
    int net_dir = 0;
    int batt_dir = 0;

    /* Net (grid): signed - positive (this app's existing P1/HomeWizard
     * convention, see data_sources.py) means importing, negative means
     * exporting. Net-Home animeert VOORTAAN ALLEEN import (Net->Home) -
     * nooit meer export, want dat zou een Home->Net-animatie vereisen en
     * Home genereert nooit (zie de layout-comment bovenaan dit bestand).
     * Export wordt getekend in de "Derived routing edges"-sectie
     * verderop, via Zon-Net en/of Batterij-Net. Weergave: twee losse
     * regels (export/import), altijd allebei zichtbaar (0 als inactief)
     * met een ASCII-pijltje - zelfde stijl als HA's eigen energy-flow
     * card (zie het door Jeroen aangeleverde screenshot), maar met "<-"/
     * "->" i.p.v. Unicode-pijlen omdat lv_font_signage_* alleen ASCII
     * dekt (zie comment bovenaan dit bestand). "<-" (export) staat boven
     * "->" (import) - zelfde volgorde als in dat screenshot. Dit label-
     * getal blijft ongewijzigd door de routing - het is nog steeds
     * "hoeveel W" ongeacht "via welke lijn getekend". */
    if(state->grid_power_w_valid) {
        if(state->grid_power_w > FLOW_DEADBAND_W) net_dir = 1;
        else if(state->grid_power_w < -FLOW_DEADBAND_W) net_dir = -1;

        bool net_home_active = net_dir == 1;
        if((net_home_active ? 1 : 0) != g_net_home_applied) {
            if(net_home_active) set_flow_dot(&g_net_anim, g_net_dot, net_dot_exec_cb, true);
            else clear_flow_dot(g_net_dot);
            lv_obj_set_style_line_color(g_net_line, net_home_active ? net_color() : dim_color(), 0);
            g_net_home_applied = net_home_active ? 1 : 0;
        }
        if(net_dir != g_net_dir_applied) {
            lv_obj_set_style_border_color(g_net_node.circle, net_dir != 0 ? net_color() : dim_color(), 0);
            g_net_dir_applied = net_dir;
        }
        double net_export_w = state->grid_power_w < 0 ? -state->grid_power_w : 0.0;
        double net_import_w = state->grid_power_w > 0 ? state->grid_power_w : 0.0;
        snprintf(buf, sizeof(buf), "<- %.0f W\n-> %.0f W", net_export_w, net_import_w);
        lv_label_set_text(g_net_node.value_label, buf);
    }

    /* Batterij: signed - positive (this app's existing Marstek convention,
     * see CLAUDE.md/marstek_client.py) means discharging, negative means
     * charging. Batterij-Home animeert VOORTAAN ALLEEN ontladen
     * (Batterij->Home) - nooit meer laden, want dat zou een
     * Home->Batterij-animatie vereisen en Home genereert nooit (zie de
     * layout-comment bovenaan dit bestand; live-gemeld door Jeroen:
     * "batterij laadt op, maar de stroom lijkt naar het huis te gaan").
     * Laden wordt getekend in de "Derived routing edges"-sectie verderop,
     * via Zon-Batterij en/of Batterij-Net (Net->Batterij-richting).
     * g_batt_home_points is ordered Batterij-side (index 0) -> Home-side
     * (index CURVE_POINTS-1), dus `forward=true` (0->N-1) is precies de
     * ontlaad-richting die hier nog over is. Weergave: zelfde twee-
     * regels-met-pijltje-stijl als Net hierboven - "v" (laden, energie
     * "in" de cel) boven "^" (ontladen, energie "uit" de cel), matcht de
     * volgorde in Jeroen's screenshot. */
    if(state->battery_power_w_valid) {
        if(state->battery_power_w > FLOW_DEADBAND_W) batt_dir = 1;
        else if(state->battery_power_w < -FLOW_DEADBAND_W) batt_dir = -1;

        bool batt_home_active = batt_dir == 1;
        if((batt_home_active ? 1 : 0) != g_batt_home_applied) {
            if(batt_home_active) set_flow_dot(&g_batt_anim, g_batt_dot, batt_dot_exec_cb, true);
            else clear_flow_dot(g_batt_dot);
            lv_obj_set_style_line_color(g_batt_line, batt_home_active ? batt_color() : dim_color(), 0);
            g_batt_home_applied = batt_home_active ? 1 : 0;
        }
        if(batt_dir != g_batt_dir_applied) {
            lv_obj_set_style_border_color(g_batt_node.circle, batt_dir != 0 ? batt_color() : dim_color(), 0);
            g_batt_dir_applied = batt_dir;
        }
        double batt_charge_w = state->battery_power_w < 0 ? -state->battery_power_w : 0.0;
        double batt_discharge_w = state->battery_power_w > 0 ? state->battery_power_w : 0.0;
        snprintf(buf, sizeof(buf), "v %.0f W\n^ %.0f W", batt_charge_w, batt_discharge_w);
        lv_label_set_text(g_batt_node.value_label, buf);
    }
    if(state->battery_soc_pct_valid) {
        lv_label_set_text_static(g_batt_node.icon, battery_icon_for_soc(state->battery_soc_pct));
        snprintf(buf, sizeof(buf), "Batterij %d%%", state->battery_soc_pct);
        lv_label_set_text(g_batt_node.name_label, buf);
    }

    /* Derived routing edges (Zon-Net, Zon-Batterij, Batterij-Net) - see
     * the layout comment at the top of this file for the full reasoning.
     * Booleans/thresholds, not a measured split: multiple of these can be
     * active at once (e.g. Zon covering Home AND exporting AND charging
     * the battery simultaneously). Batterij-Net is the one edge with 2
     * possible directions, so it's tri-state (-1/0/1) rather than a
     * simple boolean like the other two. */
    bool zon_net_active = net_dir == -1 && zon_active;
    if((zon_net_active ? 1 : 0) != g_zon_net_applied) {
        if(zon_net_active) set_flow_dot(&g_zon_net_anim, g_zon_net_dot, zon_net_dot_exec_cb, true);
        else clear_flow_dot(g_zon_net_dot);
        lv_obj_set_style_line_color(g_zon_net_line, zon_net_active ? net_color() : dim_color(), 0);
        g_zon_net_applied = zon_net_active ? 1 : 0;
    }

    bool zon_batt_active = batt_dir == -1 && zon_active;
    if((zon_batt_active ? 1 : 0) != g_zon_batt_applied) {
        if(zon_batt_active) set_flow_dot(&g_zon_batt_anim, g_zon_batt_dot, zon_batt_dot_exec_cb, true);
        else clear_flow_dot(g_zon_batt_dot);
        lv_obj_set_style_line_color(g_zon_batt_line, zon_batt_active ? batt_color() : dim_color(), 0);
        g_zon_batt_applied = zon_batt_active ? 1 : 0;
    }

    /* g_batt_net_points is ordered Batterij-side (index 0) -> Net-side
     * (last index, see screen_energyflow_build()) - so forward=true
     * (0->N-1) is Batterij->Net (discharge surplus exported), forward=
     * false is Net->Batterij (grid import charging the battery). */
    int batt_net_route = 0;
    if(net_dir == -1 && batt_dir == 1) batt_net_route = 1;       /* Batterij->Net */
    else if(net_dir == 1 && batt_dir == -1) batt_net_route = -1; /* Net->Batterij */
    if(batt_net_route != g_batt_net_applied) {
        if(batt_net_route != 0) set_flow_dot(&g_batt_net_anim, g_batt_net_dot, batt_net_dot_exec_cb, batt_net_route > 0);
        else clear_flow_dot(g_batt_net_dot);
        lv_obj_set_style_line_color(g_batt_net_line, batt_net_route != 0 ? net_color() : dim_color(), 0);
        g_batt_net_applied = batt_net_route;
    }

    /* Home: not its own IPC field - derived locally from the same 3
     * values shown in the other 3 nodes, using this app's existing sign
     * conventions (zon always >=0, grid +import/-export, battery
     * +discharge/-charge), all of which sum to "power flowing into the
     * home" when they're internally consistent. Uses `zon_forecast_w`
     * (the same forecast-curve value the Zon node itself displays), NOT
     * `state->solar_power_w` - an earlier version of this used
     * solar_power_w here, which is structurally always ~0 (no live solar
     * sensor exists, see the comment at g_net_node/g_zon_node's
     * declaration above) and made Home silently ignore the very number
     * shown on screen at Zon, live-caught by Jeroen: Zon showed 358W,
     * Net showed 229W export, so Home should read 358-229=129W, but kept
     * showing 0W because it was actually computing ~0+(-229)+0, clamped
     * up from negative. Display-only, see screen_energyflow.h's docstring
     * for why this isn't the same as actually solving storage.py's
     * documented household-load gap. Border stays home_color()
     * permanently (set once in screen_energyflow_build(), never touched
     * here) - Home is a sink, not a source, so it has no "flow direction"
     * of its own to color it by (unlike the other 3 nodes above). Same
     * "skip instead of blanking out on a transient gap" reasoning as the
     * 3 branches above - if even one of the 3 inputs is momentarily
     * invalid, just leave the last computed total showing rather than
     * replacing a real number with "-". */
    /* Geen "~"-prefix op de Zon-waarde (eerdere versie deed dat wel) - op
     * het echte scherm rendert het tilde-glyph van lv_font_signage_* zo
     * plat dat het leest als een minteken, wat Jeroen live liet denken dat
     * de zon-waarde negatief was terwijl fabs() dat altijd al uitsloot. */
    /* zon_forecast_valid is NOT required here (2026-10-01, live-caught:
     * Home showed "-" at night with a real 167W battery + 8W grid reading,
     * because solar_forecast_curve has no entry for late-night hours -
     * Forecast.Solar simply stops reporting after sunset, see
     * dashboard/solar_forecast_client.py). zon_forecast_w is already 0.0
     * whenever zon_forecast_valid is false (never left uninitialized, see
     * its declaration above) - a missing forecast is overwhelmingly more
     * likely to mean "genuinely ~0 solar right now" than "a real nonzero
     * value we're just missing", so Home can and should still compute
     * from grid+battery alone in that case rather than going blank. The
     * Zon node's OWN displayed number is handled separately below (only
     * written when zon_forecast_valid) - it still shows "-" when there's
     * no forecast (see the zon_forecast_valid block above), since THAT
     * number specifically means "no forecast data for this hour", a
     * different, still-useful signal, unlike Home's best-effort 0-default. */
    if(state->grid_power_w_valid && state->battery_power_w_valid) {
        double raw_home_w = zon_forecast_w + state->grid_power_w + state->battery_power_w;
        double home_w = raw_home_w;
        double zon_display_w = zon_forecast_w;
        /* Dynamic Home floor (2026-10-01, was the fixed MIN_HOME_W
         * constant) - state->min_home_w comes from dashboard_daemon.py
         * (storage.overnight_baseline_load_w(), see docs/ipc_protocol.md),
         * always valid (server-side default fills in). A real household
         * never actually draws 0W (standby loads) - when the raw sum
         * comes in under that floor, show the floor instead, and ADD the
         * shortfall to the Zon number specifically (not grid/batterij,
         * which are real sensor readings) - the Zon term must absorb the
         * correction in the direction that keeps zon+batterij+net ==
         * Home a real, consistent identity after the floor kicks in, not
         * just two independently-set numbers that no longer add up.
         * zon_forecast_w is >=0 by construction (solar generation), so
         * zon_display_w can only increase here - no clamping needed. */
        double min_home_w = state->min_home_w_valid ? state->min_home_w : 100.0;
        /* Only apply the floor while zon is actually contributing
         * (2026-10-12, Jeroen) - it exists to catch *invisible* solar
         * self-consumption (generation covering load without ever
         * showing up on grid/batterij), which only the estimated zon
         * term can account for. With zon_forecast_w == 0 (no sun at all,
         * not just "no forecast entry" - see zon_forecast_valid's own
         * 0-default above) grid+batterij are direct measurements that
         * already equal real household load exactly; flooring them
         * would show a fabricated number instead of the real one. */
        if(zon_forecast_w > 0.0 && raw_home_w < min_home_w) {
            double deficit = min_home_w - raw_home_w;
            home_w = min_home_w;
            zon_display_w = zon_forecast_w + deficit;
        }
        snprintf(buf, sizeof(buf), "%.0f W", home_w);
        lv_label_set_text(g_home_node.value_label, buf);
        /* Only overwrite Zon's label when there's an actual forecast to
         * correct - without one (night), zon_forecast_w/zon_display_w are
         * just a 0-default standing in for Home's math, not a real number
         * worth showing on the Zon node itself (which should keep showing
         * "-" from the zon_forecast_valid block above in that case). */
        if(zon_forecast_valid) {
            snprintf(buf, sizeof(buf), "%.0f W", zon_display_w);
            lv_label_set_text(g_zon_node.value_label, buf);
        }
    }
    else if(zon_forecast_valid) {
        /* Can't apply the min_home_w correction without grid/batterij data
         * - fall back to the raw forecast, same as before this correction
         * existed. */
        snprintf(buf, sizeof(buf), "%.0f W", zon_forecast_w);
        lv_label_set_text(g_zon_node.value_label, buf);
    }
}
