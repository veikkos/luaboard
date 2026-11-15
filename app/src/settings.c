#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "nvs.h"

static int s_brightness = DISPLAY_DEFAULT_BRIGHTNESS;
static int s_screen_off;                 /* minutes, 0 never */
static int s_turn_off;                   /* minutes asleep, 0 never */
static int s_volume = SOUND_DEFAULT_VOLUME;
static char s_autostart[VFS_APP_NAME_MAX];
static char s_tz[TZ_MAX] = TZ_DEFAULT;
static bool s_update_check = true;

size_t settings_load(const char *key, void *out, size_t cap)
{
    nvs_handle_t h;
    size_t len = cap;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return 0;
    if (nvs_get_blob(h, key, out, &len) != ESP_OK) len = 0;
    nvs_close(h);
    return len;
}

esp_err_t settings_store(const char *key, const void *data, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = len ? nvs_set_blob(h, key, data, len) : nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

size_t settings_take(const char *key, void *out, size_t cap)
{
    size_t len = settings_load(key, out, cap);
    settings_store(key, NULL, 0);
    return len;
}

void settings_init(void)
{
    uint8_t b;
    if (settings_load(NVS_KEY_BRIGHTNESS, &b, 1) == 1 && b >= BRIGHTNESS_MIN && b <= 100) s_brightness = b;
    uint16_t m;
    if (settings_load(NVS_KEY_SCREEN_OFF, &m, sizeof m) == sizeof m) s_screen_off = m;
    if (settings_load(NVS_KEY_TURN_OFF, &m, sizeof m) == sizeof m) s_turn_off = m;
    if (settings_load(NVS_KEY_VOLUME, &b, 1) == 1 && b <= 100) s_volume = b;
    if (settings_load(NVS_KEY_UPDATE_CHECK, &b, 1) == 1) s_update_check = b != 0;
    char name[VFS_APP_NAME_MAX];
    size_t n = settings_load(NVS_KEY_AUTOSTART, name, sizeof name - 1);
    name[n] = 0;
    snprintf(s_autostart, sizeof s_autostart, "%s", name);
    char tz[TZ_MAX];
    n = settings_load(NVS_KEY_TIMEZONE, tz, sizeof tz - 1);
    tz[n] = 0;
    if (n) snprintf(s_tz, sizeof s_tz, "%s", tz);
}

const char *settings_timezone(void) { return s_tz; }

esp_err_t settings_set_timezone(const char *tz)
{
    snprintf(s_tz, sizeof s_tz, "%s", tz && *tz ? tz : TZ_DEFAULT);
    return settings_store(NVS_KEY_TIMEZONE, s_tz, strlen(s_tz));
}

int settings_brightness(void) { return s_brightness; }

esp_err_t settings_set_brightness(int percent)
{
    if (percent < BRIGHTNESS_MIN) percent = BRIGHTNESS_MIN;
    if (percent > 100) percent = 100;
    s_brightness = percent;
    uint8_t b = (uint8_t)percent;
    return settings_store(NVS_KEY_BRIGHTNESS, &b, 1);
}

int settings_screen_off(void) { return s_screen_off; }

esp_err_t settings_set_screen_off(int minutes)
{
    s_screen_off = minutes < 0 ? 0 : minutes > 24 * 60 ? 24 * 60 : minutes;
    uint16_t m = (uint16_t)s_screen_off;
    return settings_store(NVS_KEY_SCREEN_OFF, &m, sizeof m);
}

int settings_turn_off(void) { return s_turn_off; }

esp_err_t settings_set_turn_off(int minutes)
{
    s_turn_off = minutes < 0 ? 0 : minutes > 24 * 60 ? 24 * 60 : minutes;
    uint16_t m = (uint16_t)s_turn_off;
    return settings_store(NVS_KEY_TURN_OFF, &m, sizeof m);
}

int settings_volume(void) { return s_volume; }

esp_err_t settings_set_volume(int percent)
{
    s_volume = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    uint8_t b = (uint8_t)s_volume;
    return settings_store(NVS_KEY_VOLUME, &b, 1);
}

const char *settings_autostart(void) { return s_autostart; }

esp_err_t settings_set_autostart(const char *app)
{
    snprintf(s_autostart, sizeof s_autostart, "%s", app ? app : "");
    return settings_store(NVS_KEY_AUTOSTART, s_autostart, strlen(s_autostart));
}

bool settings_update_check(void) { return s_update_check; }

esp_err_t settings_set_update_check(bool on)
{
    s_update_check = on;
    uint8_t b = on;
    return settings_store(NVS_KEY_UPDATE_CHECK, &b, 1);
}
