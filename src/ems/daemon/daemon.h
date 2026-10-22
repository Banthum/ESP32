// Port van dashboard/dashboard_daemon.py + data_sources.py (EMS ae7029d).
//
// Verzamelt alle databronnen, bouwt elk poll-moment het volledige
// dashboardbericht (docs/ipc_protocol.md, zelfde JSON als de Pi's /api/state)
// en publiceert het op de state bus. Bewaart metingen/besparingen via storage
// en plant elke 5 minuten opnieuw.
//
// Batterijaansturing: het "enforce/verify"-blok van de Pi zit in
// logic/battery_control (cyclus voor cyclus gelijk aan de Pi, zie
// tools/control_reference.py). Twee standen, bij het bouwen gekozen:
// - Schaduw (standaard): rekent elke cyclus uit wat hij zou sturen en logt dat
//   ("command_shadow" in battery_log, "battery_control" in /api/state), maar
//   stuurt niets. Poll elke 30 s (+ jitter): de Pi pollt de batterij al.
//   battery_active_control_enabled is false in het bericht; of de Pi een
//   laad-/blokvenster aanstuurt wordt uit ES.GetMode afgeleid voor de
//   besparingsboekhouding.
// - LIVE (-DEMS_BATTERY_CONTROL_LIVE=1): stuurt de batterij aan zoals de Pi,
//   poll elke 10 s. Alleen gebruiken als de Pi-daemon uit staat.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void daemon_start(void);

// Instellingen opnieuw inlezen bij de volgende cyclus (na opslaan via een
// instellingenscherm) - vervangt "systemctl restart dashboard-daemon".
void daemon_reload_config(void);

#ifdef __cplusplus
}
#endif
