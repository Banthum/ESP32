// Port van dashboard-web/web_server.py: de website (Kosten/Verbruik/Zon/
// Batterij/Energiestromen + Instellingen) en de bijbehorende API, op dezelfde
// WebServer als http_api (poort 80). De statische bestanden zitten in de
// firmware (web_assets.h, gemaakt door tools/embed_web.py).
//
// Net als op de Pi: geen login op het LAN, ook niet voor de instellingen -
// dezelfde validatie als de touchscreen-instellingen beperkt wat er kan.
#pragma once

#include <WebServer.h>

void web_dashboard_register(WebServer & server);
