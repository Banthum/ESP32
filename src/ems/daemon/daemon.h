// Port van dashboard/dashboard_daemon.py + data_sources.py (EMS ae7029d).
//
// Verzamelt alle databronnen, bouwt elk poll-moment het volledige
// dashboardbericht (docs/ipc_protocol.md, zelfde JSON als de Pi's /api/state)
// en publiceert het op de state bus. Bewaart metingen/besparingen via storage
// en plant elke 5 minuten opnieuw.
//
// Bewuste verschillen met de Pi:
// - Poll elke 30 s (+ jitter) i.p.v. 10 s - spaart de Marstek-UDP-chip.
// - GEEN batterijaansturing: het hele "enforce/verify"-blok van de Pi vervalt,
//   battery_active_control_enabled is altijd false in het bericht.
// - Of de Pi op dit moment een laad-/blokvenster aanstuurt wordt alleen-lezend
//   afgeleid uit ES.GetMode, voor dezelfde besparingsboekhouding als de Pi.
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
