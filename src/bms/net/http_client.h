// HTTP(S) GET met dezelfde semantiek als price_client._fetch_json() op de Pi:
// korte timeout per poging, een paar pogingen, "een mislukte read is geen data".
#pragma once

#include <cJSON.h>
#include <stddef.h>

typedef struct {
    int status;    // HTTP-status, of -1 bij netwerkfout/timeout
    char * body;   // malloc'd (PSRAM), nul-getermineerd; NULL bij fout
    size_t len;
} http_response_t;

// Eén GET-poging. `user_agent` mag NULL (dan esp_http_client's default).
http_response_t http_get(const char * url, int timeout_ms, const char * user_agent, const char * accept);
void http_response_free(http_response_t * resp);

// GET + JSON-parse met `max_attempts` pogingen; zelfde contract als
// price_client._fetch_json(): netwerkfout -> opnieuw proberen, onparseerbaar
// antwoord -> meteen NULL (lost zichzelf niet op bij een retry).
cJSON * http_get_json(const char * url, int timeout_ms, int max_attempts, const char * user_agent,
                      const char * api_name);
