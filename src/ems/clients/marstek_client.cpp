// Arduino.h vóór lwip: anders botst lwip's IPADDR_NONE-macro met Arduino's
// gelijknamige IPAddress-constante.
#include <Arduino.h>

#include "marstek_client.h"

#include <lwip/sockets.h>
#include <stdio.h>
#include <string.h>

#include "../core/log.h"

static const char * TAG = "dashboard.marstek_client";

// --- call-stats (zie get_call_stats()/reset_call_stats() in marstek_client.py) ---
#define MAX_STAT_METHODS 4
typedef struct {
    char method[24];
    int success, timeout, error, unexpected_reply;
} call_stat_t;
static call_stat_t s_stats[MAX_STAT_METHODS];
static int s_stats_count = 0;

static call_stat_t * stat_for(const char * method)
{
    for(int i = 0; i < s_stats_count; i++) {
        if(strcmp(s_stats[i].method, method) == 0) {
            return &s_stats[i];
        }
    }
    if(s_stats_count == MAX_STAT_METHODS) {
        return nullptr;
    }
    call_stat_t * s = &s_stats[s_stats_count++];
    memset(s, 0, sizeof(*s));
    snprintf(s->method, sizeof(s->method), "%s", method);
    return s;
}

cJSON * marstek_get_call_stats(void)
{
    if(s_stats_count == 0) {
        return nullptr;
    }
    cJSON * out = cJSON_CreateObject();
    for(int i = 0; i < s_stats_count; i++) {
        cJSON * m = cJSON_CreateObject();
        cJSON_AddNumberToObject(m, "success", s_stats[i].success);
        cJSON_AddNumberToObject(m, "timeout", s_stats[i].timeout);
        cJSON_AddNumberToObject(m, "error", s_stats[i].error);
        cJSON_AddNumberToObject(m, "unexpected_reply", s_stats[i].unexpected_reply);
        cJSON_AddItemToObject(out, s_stats[i].method, m);
    }
    return out;
}

void marstek_reset_call_stats(void)
{
    s_stats_count = 0;
}

// Eén JSON-RPC-over-UDP-rondreis, MARSTEK_MAX_ATTEMPTS pogingen. Geeft het
// "result"-object terug (aanroeper doet cJSON_Delete op de teruggegeven
// wrapper via *root_out), of NULL. Bewust zonder `config`-parameter: de
// schrijfkant van de API wordt op de ESP32 nooit aangeroepen.
static cJSON * call(const char * host, const char * method, cJSON ** root_out)
{
    const int msg_id = 1;
    char payload[96];
    snprintf(payload, sizeof(payload), "{\"id\": %d, \"method\": \"%s\", \"params\": {\"id\": 0}}", msg_id, method);

    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(MARSTEK_PORT);
    if(inet_pton(AF_INET, host, &dest.sin_addr) != 1) {
        EMS_LOGW(TAG, "%s: invalid host %s", method, host);
        return nullptr;
    }

    call_stat_t * stat = stat_for(method);
    static char buf[4096];

    for(int attempt = 1; attempt <= MARSTEK_MAX_ATTEMPTS; attempt++) {
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if(sock < 0) {
            EMS_LOGW(TAG, "%s: socket() failed (attempt %d/%d)", method, attempt, MARSTEK_MAX_ATTEMPTS);
            if(stat) stat->error++;
            continue;
        }
        struct timeval tv = {MARSTEK_TIMEOUT_MS / 1000, (MARSTEK_TIMEOUT_MS % 1000) * 1000};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        cJSON * result = nullptr;
        if(sendto(sock, payload, strlen(payload), 0, (struct sockaddr *) &dest, sizeof(dest)) < 0) {
            EMS_LOGW(TAG, "%s: sendto failed (attempt %d/%d)", method, attempt, MARSTEK_MAX_ATTEMPTS);
            if(stat) stat->error++;
        } else {
            int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, nullptr, nullptr);
            if(n < 0) {
                EMS_LOGW(TAG, "%s: timed out (attempt %d/%d)", method, attempt, MARSTEK_MAX_ATTEMPTS);
                if(stat) stat->timeout++;
            } else {
                buf[n] = '\0';
                cJSON * msg = cJSON_Parse(buf);
                const cJSON * id = msg ? cJSON_GetObjectItemCaseSensitive(msg, "id") : nullptr;
                cJSON * res = msg ? cJSON_GetObjectItemCaseSensitive(msg, "result") : nullptr;
                if(!msg) {
                    EMS_LOGW(TAG, "%s: unparsable reply (attempt %d/%d)", method, attempt, MARSTEK_MAX_ATTEMPTS);
                    if(stat) stat->error++;
                } else if(cJSON_IsNumber(id) && id->valueint == msg_id && res) {
                    if(stat) stat->success++;
                    *root_out = msg;
                    result = res;
                } else {
                    EMS_LOGW(TAG, "%s: unexpected reply (attempt %d/%d): %.120s", method, attempt,
                             MARSTEK_MAX_ATTEMPTS, buf);
                    if(stat) stat->unexpected_reply++;
                    cJSON_Delete(msg);
                }
            }
        }
        close(sock);
        if(result) {
            return result;
        }
    }
    return nullptr;
}

static bool get_num(const cJSON * obj, const char * key, double * out)
{
    const cJSON * v = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : nullptr;
    if(!cJSON_IsNumber(v)) {
        return false;
    }
    *out = v->valuedouble;
    return true;
}

marstek_telemetry_t marstek_read_telemetry(const char * host)
{
    marstek_telemetry_t t = {};
    cJSON * bat_root = nullptr;
    cJSON * es_root = nullptr;
    cJSON * battery = call(host, "Bat.GetStatus", &bat_root);
    cJSON * es = call(host, "ES.GetStatus", &es_root);

    t.battery_soc_pct_valid = get_num(battery, "soc", &t.battery_soc_pct);

    // ongrid_power dient als batterijvermogen - zie marstek_client.py's docstring.
    double power;
    if(get_num(es, "ongrid_power", &power)) {
        t.battery_power_w_valid = true;
        t.battery_power_w = power;
        if(power > MARSTEK_IDLE_DEADBAND_W) {
            t.battery_state = "discharging";
        } else if(power < -MARSTEK_IDLE_DEADBAND_W) {
            t.battery_state = "charging";
        } else {
            t.battery_state = "idle";
        }
        t.grid_power_w_valid = true;
        t.grid_power_w = power;
    }
    t.solar_power_w_valid = get_num(es, "pv_power", &t.solar_power_w);

    cJSON_Delete(bat_root);
    cJSON_Delete(es_root);
    return t;
}

marstek_mode_t marstek_get_mode(const char * host)
{
    marstek_mode_t m = {};
    cJSON * root = nullptr;
    cJSON * res = call(host, "ES.GetMode", &root);
    if(res) {
        m.valid = true;
        const cJSON * mode = cJSON_GetObjectItemCaseSensitive(res, "mode");
        if(cJSON_IsString(mode)) {
            snprintf(m.mode, sizeof(m.mode), "%s", mode->valuestring);
        }
        m.ongrid_power_valid = get_num(res, "ongrid_power", &m.ongrid_power);
    }
    cJSON_Delete(root);
    return m;
}
