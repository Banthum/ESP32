#include "battery_control.h"

#include <math.h>
#include <string.h>

const char * ctrl_action_name(decision_action_t a)
{
    switch(a) {
        case DECISION_AUTO: return "auto";
        case DECISION_CHARGE: return "charge";
        case DECISION_BLOCK: return "block";
        default: return nullptr;
    }
}

static bool mode_is(const ctrl_mode_reading_t * r, const char * mode)
{
    return r->valid && r->has_mode && strcmp(r->mode, mode) == 0;
}

// Python: confirmed_mode is not None (en dus ook confirmed is not None)
static bool mode_known(const ctrl_mode_reading_t * r)
{
    return r->valid && r->has_mode;
}

void ctrl_init(ctrl_state_t * s, const ctrl_mode_reading_t * startup_mode)
{
    memset(s, 0, sizeof(*s));
    s->last_sent_action = DECISION_NONE;
    if(startup_mode && mode_is(startup_mode, "Passive")) {
        if(startup_mode->ongrid_power_valid && fabs(startup_mode->ongrid_power) <= CTRL_POWER_DEDUP_THRESHOLD_W) {
            s->last_sent_action = DECISION_BLOCK;
        } else {
            s->last_sent_action = DECISION_CHARGE;
        }
    }
}

void ctrl_observe_soc(ctrl_state_t * s, bool soc_valid, double soc_pct)
{
    if(soc_valid) {
        s->last_known_soc_valid = true;
        s->last_known_soc_pct = soc_pct;
    }
}

void ctrl_step(ctrl_state_t * s, const ctrl_config_t * cfg, double now_s, const plan_decision_t * raw_decision,
               const ctrl_mode_reading_t * confirmed, bool grid_power_valid, double grid_power_w,
               ctrl_step_result_t * out)
{
    memset(out, 0, sizeof(*out));
    plan_decision_t decision = *raw_decision;

    // Minimale laadsessie: eenmaal begonnen minstens MIN_CHARGE_DURATION_S op
    // vol vermogen doorladen, tenzij de batterij al vol is (SOC onbekend =
    // nog niet vol).
    if(s->last_sent_action == DECISION_CHARGE && s->charge_started_valid && decision.action != DECISION_CHARGE &&
       (!s->last_known_soc_valid || s->last_known_soc_pct < cfg->max_soc_pct) &&
       now_s - s->charge_started_s < CTRL_MIN_CHARGE_DURATION_S) {
        double remaining = CTRL_MIN_CHARGE_DURATION_S - (now_s - s->charge_started_s);
        out->charge_floor_applied = true;
        out->floor_remaining_s = remaining;
        decision.action = DECISION_CHARGE;
        decision.power_w = -cfg->max_power_w;
        decision.remaining_s = remaining;
    }

    // Zelfde vloer voor een blokvenster, tenzij de batterij al op/onder min_soc staat.
    if(s->last_sent_action == DECISION_BLOCK && s->block_started_valid && decision.action != DECISION_BLOCK &&
       (!s->last_known_soc_valid || s->last_known_soc_pct > cfg->min_soc_pct) &&
       now_s - s->block_started_s < CTRL_MIN_CHARGE_DURATION_S) {
        double remaining = CTRL_MIN_CHARGE_DURATION_S - (now_s - s->block_started_s);
        out->block_floor_applied = true;
        out->floor_remaining_s = remaining;
        decision.action = DECISION_BLOCK;
        decision.power_w = 0.0;
        decision.remaining_s = remaining;
    }

    // Verifiëren tegen de modus die de batterij zelf meldt.
    if(!confirmed->valid) {
        s->consecutive_getmode_failures++;
        out->getmode_failed = true;
    } else {
        s->consecutive_getmode_failures = 0;
    }
    out->getmode_failures = s->consecutive_getmode_failures;

    // Te lang blind: preventief opnieuw sturen, teller meteen terug.
    bool verify_blind = s->consecutive_getmode_failures >= CTRL_GETMODE_FAILURE_RESEND_THRESHOLD;
    if(verify_blind) {
        s->consecutive_getmode_failures = 0;
        out->verify_blind = true;
    }

    if(decision.action == DECISION_AUTO) {
        bool mismatch = mode_known(confirmed) && !mode_is(confirmed, "Auto");
        if(mismatch && s->last_sent_action == DECISION_AUTO) {
            out->mismatch_event = true;
        }
        if(s->last_sent_action != DECISION_AUTO || mismatch || verify_blind) {
            out->cmd.kind = CTRL_CMD_AUTO;
            out->cmd.action = DECISION_AUTO;
        }
        return;
    }

    // "charge" (-max_power_w) en "block" (0 W) zijn allebei een Passive-commando.
    double power_w = decision.power_w;

    // Blokvenster mag nog wel zon-overschot laden: wat de P1-meter exporteert
    // wordt bijgeladen, begrensd op het max-laadvermogen.
    if(decision.action == DECISION_BLOCK) {
        if(grid_power_valid && grid_power_w < -CTRL_POWER_DEDUP_THRESHOLD_W &&
           (!s->last_known_soc_valid || s->last_known_soc_pct < cfg->max_soc_pct)) {
            power_w = -fmin(-grid_power_w, cfg->max_power_w);
        }
    }

    bool mode_mismatch = mode_known(confirmed) && !mode_is(confirmed, "Passive");
    // "Passive" gemeld maar ~0 W geleverd: alleen het label controleren mist dat.
    bool power_mismatch = confirmed->valid && !mode_mismatch && confirmed->ongrid_power_valid &&
                          fabs(confirmed->ongrid_power - power_w) > CTRL_POWER_DEDUP_THRESHOLD_W;
    bool mismatch = mode_mismatch || power_mismatch;
    bool changed = s->last_sent_action != decision.action || !s->last_sent_power_valid ||
                   fabs(power_w - s->last_sent_power_w) > CTRL_POWER_DEDUP_THRESHOLD_W;
    if(mismatch && !changed) {
        out->mismatch_event = true;
        out->mismatch_expected_passive = true;
        out->mismatch_is_power = !mode_mismatch;
    }
    if(changed || mismatch || verify_blind) {
        out->cmd.kind = CTRL_CMD_PASSIVE;
        out->cmd.action = decision.action;
        out->cmd.power_w = power_w;
        out->cmd.duration_s = fmax((double) CTRL_COMMAND_DURATION_FLOOR_S, decision.remaining_s);
    }
}

void ctrl_on_ack(ctrl_state_t * s, const ctrl_command_t * cmd, bool ack, double now_s)
{
    if(!ack || cmd->kind == CTRL_CMD_NONE) {
        return;
    }
    if(cmd->kind == CTRL_CMD_AUTO) {
        s->last_sent_action = DECISION_AUTO;
        s->last_sent_power_valid = true;
        s->last_sent_power_w = 0.0;
        s->charge_started_valid = false;
        s->block_started_valid = false;
        return;
    }
    if(cmd->action == DECISION_CHARGE && s->last_sent_action != DECISION_CHARGE) {
        s->charge_started_valid = true;
        s->charge_started_s = now_s;
    } else if(cmd->action == DECISION_BLOCK && s->last_sent_action != DECISION_BLOCK) {
        s->block_started_valid = true;
        s->block_started_s = now_s;
    }
    s->last_sent_action = cmd->action;
    s->last_sent_power_valid = true;
    s->last_sent_power_w = cmd->power_w;
}
