#include "secrets.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "nvs.h"

/* NVS keys are at most 15 characters and NVS can't enumerate them portably
   here, so each value sits under a slot key ("s00".."s31") and an index entry
   maps "<app>\t<name>\t<slot>\n" lines to them. */
#define INDEX_KEY "_index"

typedef struct {
    char app[VFS_APP_NAME_MAX];
    char name[16];
    int slot;
} entry_t;

typedef struct {
    entry_t e[SECRET_MAX_COUNT];
    int n;
} index_t;

bool secrets_valid_name(const char *name)
{
    size_t n = strlen(name);
    if (n < 1 || n > 15 || name[0] == '_') return false;
    for (const char *p = name; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '.' && *p != '-') return false;
    return true;
}

static void slot_key(int slot, char out[8]) { snprintf(out, 8, "s%02d", slot); }

static char *read_blob(nvs_handle_t h, const char *key, size_t *len)
{
    size_t n = 0;
    if (nvs_get_blob(h, key, NULL, &n) != ESP_OK) return NULL;
    char *buf = malloc(n + 1);
    if (!buf) return NULL;
    if (nvs_get_blob(h, key, buf, &n) != ESP_OK) { free(buf); return NULL; }
    buf[n] = 0;
    if (len) *len = n;
    return buf;
}

static void load(nvs_handle_t h, index_t *ix)
{
    ix->n = 0;
    char *idx = read_blob(h, INDEX_KEY, NULL);
    if (!idx) return;
    for (char *p = idx; *p && ix->n < SECRET_MAX_COUNT; ) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        char *t1 = strchr(p, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        int slot = t2 && t2[1] == 's' ? atoi(t2 + 2) : -1;
        if (slot >= 0 && slot < SECRET_MAX_COUNT) {   /* a damaged line is skipped */
            entry_t *x = &ix->e[ix->n++];
            snprintf(x->app, sizeof x->app, "%.*s", (int)(t1 - p), p);
            snprintf(x->name, sizeof x->name, "%.*s", (int)(t2 - t1 - 1), t1 + 1);
            x->slot = slot;
        }
        if (!e) break;
        p = e + 1;
    }
    free(idx);
}

static esp_err_t save(nvs_handle_t h, const index_t *ix)
{
    size_t cap = (size_t)ix->n * (VFS_APP_NAME_MAX + 16 + 8) + 1, o = 0;
    char *buf = malloc(cap);
    if (!buf) return ESP_ERR_NO_MEM;
    buf[0] = 0;
    for (int i = 0; i < ix->n && o < cap; i++) {
        int k = snprintf(buf + o, cap - o, "%s\t%s\ts%02d\n", ix->e[i].app, ix->e[i].name, ix->e[i].slot);
        if (k < 0 || (size_t)k >= cap - o) { free(buf); return ESP_ERR_INVALID_SIZE; }
        o += (size_t)k;
    }
    esp_err_t err = o ? nvs_set_blob(h, INDEX_KEY, buf, o) : nvs_erase_key(h, INDEX_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    free(buf);
    return err;
}

static int find(const index_t *ix, const char *app, const char *name)
{
    for (int i = 0; i < ix->n; i++)
        if (!strcmp(ix->e[i].app, app) && !strcmp(ix->e[i].name, name)) return i;
    return -1;
}

static bool valid(const char *app, const char *name)
{
    return app && *app && strlen(app) < VFS_APP_NAME_MAX && !strpbrk(app, "\t\n") && secrets_valid_name(name);
}

/* The namespace opened and its index loaded, for index_close. */
static esp_err_t index_open(nvs_open_mode_t mode, nvs_handle_t *h, index_t **ix)
{
    esp_err_t err = nvs_open(NVS_SECRETS_NS, mode, h);
    if (err != ESP_OK) return err;
    if (!(*ix = malloc(sizeof **ix))) {
        nvs_close(*h);
        return ESP_ERR_NO_MEM;
    }
    load(*h, *ix);
    return ESP_OK;
}

/* Free the index, close the namespace; returns err. */
static esp_err_t index_close(nvs_handle_t h, index_t *ix, esp_err_t err)
{
    free(ix);
    nvs_close(h);
    return err;
}

/* The size of the value in slot; false if there is none. */
static bool slot_size(nvs_handle_t h, int slot, size_t *n)
{
    char key[8];
    slot_key(slot, key);
    *n = 0;
    return nvs_get_blob(h, key, NULL, n) == ESP_OK;
}

char *secrets_get(const char *app, const char *name, size_t *len)
{
    nvs_handle_t h;
    index_t *ix;
    if (!valid(app, name) || index_open(NVS_READONLY, &h, &ix) != ESP_OK) return NULL;
    int i = find(ix, app, name);
    char key[8], *v = NULL;
    if (i >= 0) {
        slot_key(ix->e[i].slot, key);
        v = read_blob(h, key, len);
    }
    index_close(h, ix, ESP_OK);
    return v;
}

/* Bytes stored for one app (app != NULL) or all, not counting entry skip. */
static size_t stored_bytes(nvs_handle_t h, const index_t *ix, const char *app, int skip)
{
    size_t sum = 0, n;
    for (int k = 0; k < ix->n; k++)
        if (k != skip && (!app || !strcmp(ix->e[k].app, app)) && slot_size(h, ix->e[k].slot, &n)) sum += n;
    return sum;
}

/* A new entry for app/name in a free slot; -1 if there's no room. */
static int add_entry(index_t *ix, const char *app, const char *name)
{
    if (ix->n >= SECRET_MAX_COUNT) return -1;
    bool used[SECRET_MAX_COUNT] = { false };
    for (int k = 0; k < ix->n; k++)
        if (ix->e[k].slot >= 0 && ix->e[k].slot < SECRET_MAX_COUNT) used[ix->e[k].slot] = true;
    int slot = 0;
    while (used[slot]) slot++;
    entry_t *x = &ix->e[ix->n];
    snprintf(x->app, sizeof x->app, "%s", app);
    snprintf(x->name, sizeof x->name, "%s", name);
    x->slot = slot;
    return ix->n++;
}

esp_err_t secrets_set(const char *app, const char *name, const char *value, size_t len)
{
    if (!valid(app, name)) return ESP_ERR_INVALID_ARG;
    if (value && len > SECRET_MAX_LEN) return ESP_ERR_INVALID_SIZE;
    nvs_handle_t h;
    index_t *ix;
    esp_err_t err = index_open(NVS_READWRITE, &h, &ix);
    if (err != ESP_OK) return err;
    int i = find(ix, app, name);
    char key[8];
    if (value) {
        if (stored_bytes(h, ix, app, i) + len > SECRETS_APP_BYTES ||
            stored_bytes(h, ix, NULL, i) + len > SECRETS_TOTAL_BYTES)
            return index_close(h, ix, ESP_ERR_NVS_NOT_ENOUGH_SPACE);
        if (i < 0 && (i = add_entry(ix, app, name)) < 0) return index_close(h, ix, ESP_ERR_NO_MEM);
        slot_key(ix->e[i].slot, key);
        err = nvs_set_blob(h, key, value, len);
    } else if (i >= 0) {
        slot_key(ix->e[i].slot, key);
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        ix->e[i] = ix->e[--ix->n];
    } else {
        return index_close(h, ix, ESP_OK);            /* nothing to delete */
    }
    if (err == ESP_OK) err = save(h, ix);
    if (err == ESP_OK) err = nvs_commit(h);
    return index_close(h, ix, err);
}

int secrets_list(const char *app, void (*fn)(const char *app, const char *name, size_t len, void *ctx), void *ctx)
{
    nvs_handle_t h;
    index_t *ix;
    if (index_open(NVS_READONLY, &h, &ix) != ESP_OK) return 0;
    int n = 0;
    size_t len;
    for (int i = 0; i < ix->n; i++) {
        if ((app && strcmp(ix->e[i].app, app)) || !slot_size(h, ix->e[i].slot, &len)) continue;
        fn(ix->e[i].app, ix->e[i].name, len, ctx);
        n++;
    }
    index_close(h, ix, ESP_OK);
    return n;
}

esp_err_t secrets_clear(const char *app)
{
    if (!app || !*app) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    index_t *ix;
    esp_err_t err = index_open(NVS_READWRITE, &h, &ix);
    if (err != ESP_OK) return err;
    int kept = 0;
    for (int i = 0; i < ix->n; i++) {
        if (!strcmp(ix->e[i].app, app)) {
            char key[8];
            slot_key(ix->e[i].slot, key);
            nvs_erase_key(h, key);
        } else {
            ix->e[kept++] = ix->e[i];
        }
    }
    ix->n = kept;
    err = save(h, ix);
    if (err == ESP_OK) err = nvs_commit(h);
    return index_close(h, ix, err);
}

esp_err_t secrets_wipe(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_SECRETS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);     /* the whole namespace, listed in the index or not */
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
