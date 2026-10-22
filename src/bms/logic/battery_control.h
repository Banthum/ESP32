// Port van het "enforce/verify"-blok uit dashboard/dashboard_daemon.py (EMS
// ae7029d, main(): de opstart-initialisatie van last_sent_action en het blok
// "if battery_active_control_enabled and raw_decision is not None:").
//
// Pure toestandsmachine zonder I/O: de daemon geeft per cyclus de beslissing
// van de planner, de ES.GetMode-uitlezing en de P1-meting mee, en krijgt terug
// welk commando (Auto of Passive) er naar de batterij moet. Na het versturen
// meldt de daemon met ctrl_on_ack() of de batterij het bevestigde - pas dan
// verandert last_sent_action, net als op de Pi.
//
// Zo is de logica exact te vergelijken met de Python-code van de Pi:
// tools/control_reference.py draait het originele Python-blok op dezelfde
// invoer en schrijft de verwachte uitkomsten naar test/test_bms_logic.
#pragma once

#include <stdbool.h>

#include "battery_scheduler.h"

// --- constanten uit dashboard_daemon.py (zelfde waarden) ---
#define CTRL_REPLAN_INTERVAL_S (5 * 60)
#define CTRL_COMMAND_DURATION_FLOOR_S (3 * CTRL_REPLAN_INTERVAL_S)
#define CTRL_MIN_CHARGE_DURATION_S (15 * 60)
#define CTRL_POWER_DEDUP_THRESHOLD_W 25.0
#define CTRL_GETMODE_FAILURE_RESEND_THRESHOLD 3

// Wat ES.GetMode teruggaf (Python: het "result"-dict of None).
typedef struct {
    bool valid;      // false = get_mode() gaf None
    bool has_mode;   // false = geen "mode"-veld (confirmed.get("mode") is None)
    char mode[16];   // "Auto"/"Passive"/...
    bool ongrid_power_valid;
    double ongrid_power;
} ctrl_mode_reading_t;

typedef struct {
    double max_power_w;
    double min_soc_pct;
    double max_soc_pct;
} ctrl_config_t;

typedef struct {
    decision_action_t last_sent_action; // DECISION_NONE = nog niets verstuurd (Python None)
    bool last_sent_power_valid;
    double last_sent_power_w;
    int consecutive_getmode_failures;
    bool charge_started_valid;
    double charge_started_s; // monotone tijd
    bool block_started_valid;
    double block_started_s;
    bool last_known_soc_valid;
    double last_known_soc_pct;
} ctrl_state_t;

typedef enum { CTRL_CMD_NONE = 0, CTRL_CMD_AUTO, CTRL_CMD_PASSIVE } ctrl_cmd_kind_t;

typedef struct {
    ctrl_cmd_kind_t kind;
    decision_action_t action; // AUTO, CHARGE of BLOCK
    double power_w;           // - = laden; alleen bij PASSIVE
    double duration_s;        // cd_time zoals de Pi het logt (verstuurd als int)
} ctrl_command_t;

// Alles wat de Pi in deze cyclus logt of als battery_log-event wegschrijft.
typedef struct {
    ctrl_command_t cmd;
    bool charge_floor_applied; // "minimale laadduur nog niet bereikt"
    bool block_floor_applied;  // "minimale blokkeerduur nog niet bereikt"
    double floor_remaining_s;
    bool getmode_failed;       // + consecutive_getmode_failures in de toestand
    int getmode_failures;      // teller zoals gelogd (vóór een eventuele reset)
    bool verify_blind;         // preventief opnieuw sturen
    bool mismatch_event;       // battery_log "mode_mismatch"
    bool mismatch_expected_passive;
    bool mismatch_is_power;    // Passive bevestigd, maar ongrid_power wijkt af
} ctrl_step_result_t;

// Opstart: last_sent_action uit de actuele modus van de batterij (Passive +
// ~0 W = blok, Passive anders = laden), zodat een herstart midden in een
// commando dat niet als "nog niets verstuurd" behandelt.
void ctrl_init(ctrl_state_t * s, const ctrl_mode_reading_t * startup_mode);

// Elke cyclus, vóór ctrl_step (Python: last_known_soc_pct bijwerken).
void ctrl_observe_soc(ctrl_state_t * s, bool soc_valid, double soc_pct);

// Eén cyclus. Alleen aanroepen als aansturing aan staat en de planner een
// beslissing heeft (Python: battery_active_control_enabled and raw_decision
// is not None) - anders verandert er niets, ook de GetMode-teller niet.
void ctrl_step(ctrl_state_t * s, const ctrl_config_t * cfg, double now_s, const plan_decision_t * raw_decision,
               const ctrl_mode_reading_t * confirmed, bool grid_power_valid, double grid_power_w,
               ctrl_step_result_t * out);

// Na het versturen van out->cmd (alleen als kind != CTRL_CMD_NONE).
void ctrl_on_ack(ctrl_state_t * s, const ctrl_command_t * cmd, bool ack, double now_s);

const char * ctrl_action_name(decision_action_t a); // "auto"/"charge"/"block"/NULL
