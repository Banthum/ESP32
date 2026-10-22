// Port van dashboard-ui/solar_settings_writer.py (+ de PDOK-geocoding uit
// display/solar_forecast.py): Zon-instellingen valideren, adres geocoden en
// solar_config.json schrijven. Zelfde foutteksten, die de UI letterlijk toont.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// PDOK Locatieserver: postcode + huisnummer -> lat/lon. false + reden bij fout.
bool solar_geocode(const char * postcode, const char * huisnummer, double * lat, double * lon, char * error_out,
                   size_t error_out_size);

// De 8-argumenten-variant van solar_settings_writer.py (strings zoals ingetypt).
bool solar_settings_write(const char * postcode, const char * huisnummer, const char * panel_count_str,
                          const char * panel_wp_str, const char * richting, const char * forecast_factor_str,
                          const char * declination_str, const char * azimuth_str, char * error_out,
                          size_t error_out_size);

// "Geen zonnepanelen": {"no_solar_panels": true}, volledige overschrijving.
bool solar_settings_write_no_panels(char * error_out, size_t error_out_size);

#ifdef __cplusplus
}
#endif
