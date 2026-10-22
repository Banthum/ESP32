// HTTP-API op poort 80 - zelfde /api/state als dashboard-web/web_server.py
// op de Pi (poort 8080), de website zelf (web_dashboard.h), plus diagnostiek.
#pragma once

// `write_token`: vereist in header "X-BMS-Token" voor alles wat schrijft
// (config zetten, importeren). Lezen (/api/state e.d.) is open op het LAN,
// net als web_server.py op de Pi.
void http_api_start(const char * write_token);
