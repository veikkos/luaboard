#include "wifilink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth.h"
#include "esp_log.h"
#include "esp_random.h"
#include "hex.h"
#include "hw.h"
#include "psa/crypto.h"

static const char *TAG = "wifilink";

#define TAG_LEN   16
#define FRAME_MAX (2 + WL_MAX_PLAIN + TAG_LEN)
#define HELLO_MAX 128

bool wl_open(wl_t *w)
{
    wl_close(w);
    w->in = hw_big_alloc(FRAME_MAX);
    w->plain = hw_big_alloc(WL_MAX_PLAIN);
    if (!w->in || !w->plain) { wl_close(w); return false; }
    return true;
}

void wl_close(wl_t *w)
{
    if (w->krx) psa_destroy_key(w->krx);
    if (w->ktx) psa_destroy_key(w->ktx);
    free(w->in);
    free(w->plain);
    memset(w, 0, sizeof *w);
}

uint8_t *wl_room(wl_t *w, size_t *room)
{
    *room = w->in ? FRAME_MAX - w->in_len : 0;
    return w->in ? w->in + w->in_len : NULL;
}

static bool import_key(const uint8_t k[32], uint32_t *id)
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&a, PSA_ALG_GCM);
    psa_key_id_t kid = 0;
    psa_status_t st = psa_import_key(&a, k, 32, &kid);
    *id = kid;
    return st == PSA_SUCCESS;
}

static void say(wl_send_fn send, void *ctx, const char *line) { send(ctx, line, strlen(line)); }

/* "HELLO 1 <32 hex>": answer it and switch to frames. */
static bool hello(wl_t *w, char *line, wl_send_fn send, void *ctx)
{
    uint8_t cn[AUTH_NONCE_LEN];
    bool ok = !strncmp(line, "HELLO ", 6);
    if (!ok) {
        say(send, ctx, "ERR this link is encrypted: start with HELLO (update the PC tools; see docs/PROTOCOL.md)\n");
        return false;
    }
    if (strncmp(line + 6, "1 ", 2)) { say(send, ctx, "ERR unsupported link version (this device speaks 1)\n"); return false; }
    if (hex_decode(line + 8, cn, sizeof cn) != (long)sizeof cn) { say(send, ctx, "ERR usage: HELLO 1 <32 hex digits>\n"); return false; }
    if (!auth_paired()) {
        say(send, ctx, "ERR not paired: the Wi-Fi link needs pairing over USB first (luaboard pair)\n");
        return false;
    }
    static bool psa_ready;
    if (!psa_ready) psa_ready = psa_crypto_init() == PSA_SUCCESS;
    uint8_t sn[AUTH_NONCE_LEN], c2s[32], s2c[32];
    esp_fill_random(sn, sizeof sn);
    auth_session_keys(cn, sn, c2s, s2c);
    ok = psa_ready && import_key(c2s, &w->krx) && import_key(s2c, &w->ktx);
    memset(c2s, 0, sizeof c2s);
    memset(s2c, 0, sizeof s2c);
    if (!ok) { say(send, ctx, "ERR the device couldn't set up encryption\n"); return false; }
    char id[AUTH_ID_HEX], snhex[2 * AUTH_NONCE_LEN + 1], reply[80];
    auth_key_id(id);
    hex_encode(sn, sizeof sn, snhex);
    snprintf(reply, sizeof reply, "OK HELLO 1 %s %s\n", id, snhex);
    say(send, ctx, reply);
    w->state = WL_SECURE;
    return true;
}

bool wl_received(wl_t *w, size_t n, wl_send_fn send, void *ctx)
{
    w->in_len += n;
    if (w->state != WL_HELLO) return true;
    uint8_t *nl = memchr(w->in, '\n', w->in_len);
    if (!nl) {
        if (w->in_len < HELLO_MAX) return true;
        say(send, ctx, "ERR this link is encrypted: start with HELLO (see docs/PROTOCOL.md)\n");
        return false;
    }
    size_t len = (size_t)(nl - w->in);
    if (len >= HELLO_MAX) return false;
    char line[HELLO_MAX];
    memcpy(line, w->in, len);
    line[len] = 0;
    if (len && line[len - 1] == '\r') line[len - 1] = 0;
    w->in_len -= len + 1;
    memmove(w->in, nl + 1, w->in_len);          /* a first frame may follow right away */
    return hello(w, line, send, ctx);
}

static void nonce_of(uint64_t count, uint8_t nonce[12])
{
    memset(nonce, 0, 4);
    for (int i = 0; i < 8; i++) nonce[4 + i] = (uint8_t)(count >> (56 - 8 * i));
}

int wl_read(wl_t *w, uint8_t *buf, size_t cap)
{
    if (w->plain_off == w->plain_len && w->state == WL_SECURE && w->in_len >= 2) {
        size_t clen = (size_t)w->in[0] << 8 | w->in[1];
        if (clen <= TAG_LEN || clen > WL_MAX_PLAIN + TAG_LEN) {
            ESP_LOGW(TAG, "bad frame length %u", (unsigned)clen);
            return -1;
        }
        if (w->in_len >= 2 + clen) {
            uint8_t nonce[12];
            nonce_of(w->nrx, nonce);
            size_t out = 0;
            psa_status_t st = psa_aead_decrypt(w->krx, PSA_ALG_GCM, nonce, sizeof nonce, w->in, 2,
                                               w->in + 2, clen, w->plain, WL_MAX_PLAIN, &out);
            if (st != PSA_SUCCESS) {
                ESP_LOGW(TAG, "a frame didn't decrypt: wrong key, or tampered with");
                return -1;
            }
            w->nrx++;
            w->verified = true;
            w->plain_len = out;
            w->plain_off = 0;
            w->in_len -= 2 + clen;
            memmove(w->in, w->in + 2 + clen, w->in_len);
        }
    }
    size_t n = w->plain_len - w->plain_off;
    if (n > cap) n = cap;
    memcpy(buf, w->plain + w->plain_off, n);
    w->plain_off += n;
    return (int)n;
}

void wl_write(wl_t *w, const void *data, size_t len, wl_send_fn send, void *ctx)
{
    static uint8_t frame[FRAME_MAX];            /* main loop only */
    const uint8_t *p = data;
    if (w->state != WL_SECURE) return;
    while (len > 0 && w->state == WL_SECURE) {         /* sending can close the link */
        size_t n = len < WL_MAX_PLAIN ? len : WL_MAX_PLAIN;
        size_t clen = n + TAG_LEN, out = 0;
        frame[0] = (uint8_t)(clen >> 8);
        frame[1] = (uint8_t)clen;
        uint8_t nonce[12];
        nonce_of(w->ntx, nonce);
        if (psa_aead_encrypt(w->ktx, PSA_ALG_GCM, nonce, sizeof nonce, frame, 2, p, n,
                             frame + 2, sizeof frame - 2, &out) != PSA_SUCCESS || out != clen) {
            ESP_LOGE(TAG, "encrypting failed");
            return;
        }
        w->ntx++;
        send(ctx, frame, 2 + clen);
        p += n;
        len -= n;
    }
}
