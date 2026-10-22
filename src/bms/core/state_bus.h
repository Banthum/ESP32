// In-process vervanger van de Unix-socket uit docs/ipc_protocol.md: de
// daemon-taak publiceert elk volledig dashboardbericht (zelfde JSON-schema),
// de UI en de HTTP-API (/api/state) lezen de laatste versie.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Kopieert `json` (nul-getermineerd) als nieuwste bericht.
void state_bus_publish(const char * json);

// Kopie van het nieuwste bericht (malloc'd, aanroeper free't), of NULL als er
// nog nooit iets gepubliceerd is. `version_out` (optioneel) telt op per publish.
char * state_bus_copy(uint32_t * version_out);

uint32_t state_bus_version(void);

#ifdef __cplusplus
}
#endif
