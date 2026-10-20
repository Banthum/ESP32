// Arduino.h vóór de ESP-IDF/lwip-headers (zie marstek_client.cpp).
#include <Arduino.h>

#include "http_client.h"

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <stdlib.h>
#include <string.h>

#include "../core/log.h"

static const char * TAG = "http_client";

typedef struct {
    char * buf;
    size_t len;
    size_t cap;
} body_buf_t;

static esp_err_t on_event(esp_http_client_event_t * evt)
{
    if(evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    body_buf_t * b = (body_buf_t *) evt->user_data;
    size_t need = b->len + evt->data_len + 1;
    if(need > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while(cap < need) {
            cap *= 2;
        }
        char * nb = (char *) heap_caps_realloc(b->buf, cap, MALLOC_CAP_SPIRAM);
        if(!nb) {
            return ESP_FAIL;
        }
        b->buf = nb;
        b->cap = cap;
    }
    memcpy(b->buf + b->len, evt->data, evt->data_len);
    b->len += evt->data_len;
    b->buf[b->len] = '\0';
    return ESP_OK;
}

http_response_t http_get(const char * url, int timeout_ms, const char * user_agent, const char * accept)
{
    http_response_t resp = {-1, nullptr, 0};
    body_buf_t body = {nullptr, 0, 0};

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = timeout_ms;
    cfg.event_handler = on_event;
    cfg.user_data = &body;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.user_agent = user_agent;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 1024;
    cfg.disable_auto_redirect = false;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if(!client) {
        return resp;
    }
    if(accept) {
        esp_http_client_set_header(client, "Accept", accept);
    }
    esp_err_t err = esp_http_client_perform(client);
    if(err == ESP_OK) {
        resp.status = esp_http_client_get_status_code(client);
        resp.body = body.buf;
        resp.len = body.len;
        if(!resp.body) {
            resp.body = (char *) calloc(1, 1);
        }
    } else {
        EMS_LOGW(TAG, "GET %s: %s", url, esp_err_to_name(err));
        free(body.buf);
    }
    esp_http_client_cleanup(client);
    return resp;
}

void http_response_free(http_response_t * resp)
{
    free(resp->body);
    resp->body = nullptr;
    resp->len = 0;
}

cJSON * http_get_json(const char * url, int timeout_ms, int max_attempts, const char * user_agent,
                      const char * api_name)
{
    for(int attempt = 1; attempt <= max_attempts; attempt++) {
        http_response_t r = http_get(url, timeout_ms, user_agent, "application/json");
        if(r.status < 0) {
            EMS_LOGW(TAG, "could not reach %s (attempt %d/%d)", api_name, attempt, max_attempts);
            continue;
        }
        if(r.status != 200) {
            // urllib.urlopen() gooit HTTPError bij !=2xx, wat price_client.py
            // als URLError behandelt -> opnieuw proberen.
            EMS_LOGW(TAG, "could not reach %s (attempt %d/%d): HTTP %d", api_name, attempt, max_attempts, r.status);
            http_response_free(&r);
            continue;
        }
        cJSON * json = cJSON_Parse(r.body);
        http_response_free(&r);
        if(!json) {
            EMS_LOGW(TAG, "unexpected %s response (attempt %d/%d): not JSON", api_name, attempt, max_attempts);
            return nullptr;
        }
        return json;
    }
    return nullptr;
}
