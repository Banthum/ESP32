// Port van dashboard/p1_client.py: HomeWizard WiFi-P1-meter via z'n Local API.
#pragma once

#include <stdbool.h>

// Live netvermogen (W, + = import, - = export). Cachet het apparaatadres
// (eerst uit p1_source.json, anders mDNS-discovery); bij een mislukte read
// met cache: één keer opnieuw zoeken, zodat een nieuw DHCP-adres herstelt.
bool p1_read_grid_power_w(double * out_w);

// Het nu bekende meteradres ("" als onbekend) - voor diagnostiek.
const char * p1_current_ip(void);

// Voor de onboarding (port van display/wifip1_discovery.py): zoek een meter,
// vul ip/port/path. true als gevonden met api_enabled=1.
bool p1_discover(char * ip, int ip_size, int * port, char * path, int path_size);
