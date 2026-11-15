/*
 * Secrets: small named values (tokens, API keys) kept in NVS. Each secret
 * belongs to an app: a script only reaches its own app's secrets, and
 * uninstalling or clearing an app deletes them. The protocol can list names,
 * set and delete them, but never read values back.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/** 1..15 characters of A-Z a-z 0-9 _ . - */
bool secrets_valid_name(const char *name);
/** Value into a malloc'd buffer (caller frees), NULL if not set. */
char *secrets_get(const char *app, const char *name, size_t *len);
/** Store; value NULL deletes. ESP_ERR_INVALID_SIZE: too long, ESP_ERR_NO_MEM:
 *  too many, ESP_ERR_NVS_NOT_ENOUGH_SPACE: over SECRETS_APP_BYTES for the
 *  app or SECRETS_TOTAL_BYTES in all. */
esp_err_t secrets_set(const char *app, const char *name, const char *value, size_t len);
/** One call per secret, of one app (app != NULL) or all. Returns the count. */
int secrets_list(const char *app, void (*fn)(const char *app, const char *name, size_t len, void *ctx), void *ctx);
/** Delete every secret of an app. */
esp_err_t secrets_clear(const char *app);
/** Delete all secrets. */
esp_err_t secrets_wipe(void);
