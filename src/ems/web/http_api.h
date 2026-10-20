// HTTP-API op poort 80 - zelfde /api/state als dashboard-web/web_server.py
// op de Pi (poort 8080), plus diagnostiek voor ontwikkeling.
#pragma once

// `write_token`: vereist in header "X-EMS-Token" voor alles wat schrijft
// (config zetten, importeren). Lezen (/api/state e.d.) is open op het LAN,
// net als web_server.py op de Pi.
void http_api_start(const char * write_token);
