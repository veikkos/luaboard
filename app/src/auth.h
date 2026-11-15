/*
 * Pairing for the Wi-Fi link. USB is always trusted (physical access);
 * without pairing the Wi-Fi link takes no commands at all.
 *
 * Pairing, over USB only, makes a random 256-bit key K with a random id. The
 * PC tools keep K (by id) and the device keeps it in NVS. Each Wi-Fi
 * connection then starts with HELLO (wifilink.c): both sides send a fresh
 * random nonce and derive this connection's keys from K and the nonces;
 * everything after is AES-256-GCM frames. Only someone holding K can give the
 * device commands or read what it says, a PC knows it reached the real
 * device, and nothing recorded can be replayed or used to guess K.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define AUTH_KEY_LEN     32
#define AUTH_ID_LEN      8
#define AUTH_ID_HEX      (2 * AUTH_ID_LEN + 1)
#define AUTH_NONCE_LEN   16

void auth_init(void);
/** A key is set: the Wi-Fi link can be used. */
bool auth_paired(void);
/** The key's id as hex ("" if not paired). */
void auth_key_id(char out[AUTH_ID_HEX]);
/** The key itself, for pairing another computer over USB. False if not paired. */
bool auth_key(uint8_t out[AUTH_KEY_LEN]);
/** Make a new key (and id), replacing any old one. */
esp_err_t auth_pair_new(void);
/** Forget the key: the Wi-Fi link takes no commands. */
esp_err_t auth_unpair(void);
/** This connection's keys, client-to-device and device-to-client:
 *  prk = HMAC-SHA256(K, "luaboard wifi 1" || id || cn || sn),
 *  c2s = HMAC-SHA256(prk, "c2s"), s2c = HMAC-SHA256(prk, "s2c"). */
void auth_session_keys(const uint8_t cn[AUTH_NONCE_LEN], const uint8_t sn[AUTH_NONCE_LEN],
                       uint8_t c2s[32], uint8_t s2c[32]);
