#include "auth.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "esp_log.h"
#include "esp_random.h"
#include "hex.h"
#include "settings.h"
#include "sha256.h"

static const char *TAG = "auth";

/* NVS record: version, id, key. */
#define REC_VERSION 1
#define REC_LEN (1 + AUTH_ID_LEN + AUTH_KEY_LEN)

static bool s_paired;
static uint8_t s_id[AUTH_ID_LEN];
static uint8_t s_key[AUTH_KEY_LEN];

void auth_init(void)
{
    uint8_t rec[REC_LEN];
    s_paired = settings_load(NVS_KEY_WIFI_KEY, rec, sizeof rec) == REC_LEN && rec[0] == REC_VERSION;
    if (s_paired) {
        memcpy(s_id, rec + 1, AUTH_ID_LEN);
        memcpy(s_key, rec + 1 + AUTH_ID_LEN, AUTH_KEY_LEN);
    }
    ESP_LOGI(TAG, "Wi-Fi link %s", s_paired ? "paired" : "not paired (takes no commands)");
}

bool auth_paired(void) { return s_paired; }

void auth_key_id(char out[AUTH_ID_HEX])
{
    out[0] = 0;
    if (!s_paired) return;
    hex_encode(s_id, AUTH_ID_LEN, out);
}

bool auth_key(uint8_t out[AUTH_KEY_LEN])
{
    if (!s_paired) return false;
    memcpy(out, s_key, AUTH_KEY_LEN);
    return true;
}

esp_err_t auth_pair_new(void)
{
    uint8_t rec[REC_LEN];
    rec[0] = REC_VERSION;
    esp_fill_random(rec + 1, AUTH_ID_LEN + AUTH_KEY_LEN);
    esp_err_t err = settings_store(NVS_KEY_WIFI_KEY, rec, sizeof rec);
    if (err == ESP_OK) {
        memcpy(s_id, rec + 1, AUTH_ID_LEN);
        memcpy(s_key, rec + 1 + AUTH_ID_LEN, AUTH_KEY_LEN);
        s_paired = true;
    }
    memset(rec, 0, sizeof rec);
    return err;
}

esp_err_t auth_unpair(void)
{
    esp_err_t err = settings_store(NVS_KEY_WIFI_KEY, NULL, 0);
    if (err == ESP_OK) {
        s_paired = false;
        memset(s_key, 0, sizeof s_key);
    }
    return err;
}

void auth_session_keys(const uint8_t cn[AUTH_NONCE_LEN], const uint8_t sn[AUTH_NONCE_LEN],
                       uint8_t c2s[32], uint8_t s2c[32])
{
    static const char label[] = "luaboard wifi 1";
    uint8_t msg[sizeof label - 1 + AUTH_ID_LEN + 2 * AUTH_NONCE_LEN], prk[32];
    size_t o = 0;
    memcpy(msg + o, label, sizeof label - 1); o += sizeof label - 1;
    memcpy(msg + o, s_id, AUTH_ID_LEN); o += AUTH_ID_LEN;
    memcpy(msg + o, cn, AUTH_NONCE_LEN); o += AUTH_NONCE_LEN;
    memcpy(msg + o, sn, AUTH_NONCE_LEN); o += AUTH_NONCE_LEN;
    lb_hmac_sha256(s_key, AUTH_KEY_LEN, msg, o, prk);
    lb_hmac_sha256(prk, sizeof prk, "c2s", 3, c2s);
    lb_hmac_sha256(prk, sizeof prk, "s2c", 3, s2c);
    memset(prk, 0, sizeof prk);
}
