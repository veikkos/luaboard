#include "appdata.h"

#include <string.h>

#include "app_config.h"
#include "ble.h"
#include "hw.h"
#include "script.h"
#include "secrets.h"
#include "settings.h"
#include "vfs.h"

esp_err_t appdata_clear(const char *app)
{
    vfs_data_clear(app);
    ble_forget_app(app);
    return secrets_clear(app);
}

void appdata_forget(const char *app)
{
    if (!strcmp(settings_autostart(), app)) settings_set_autostart("");
    appdata_clear(app);
}

static void skip_secret(const char *app, const char *name, size_t len, void *ctx) {}

esp_err_t app_remove(const char *app)
{
    char cur[VFS_APP_NAME_MAX];
    script_app(cur, sizeof cur);
    if (script_running() && !strcmp(cur, app)) script_stop();
    esp_err_t err = vfs_uninstall(app);
    if (err == ESP_ERR_NOT_FOUND && vfs_valid_app(app) &&
        (vfs_data_size(app) >= 0 || secrets_list(app, skip_secret, NULL) > 0)) err = ESP_OK;
    if (err == ESP_OK) appdata_forget(app);
    return err;
}

esp_err_t apps_wipe(void)
{
    script_stop();
    hw_watchdog_stretch(WATCHDOG_FORMAT_MS);      /* erasing ~13 MB takes a while */
    esp_err_t err = vfs_format();                 /* the files, the store's data, uploads */
    hw_watchdog_stretch(0);
    settings_set_autostart("");
    esp_err_t werr = secrets_wipe();              /* even if the apps' format failed */
    ble_forget_app(NULL);
    return err == ESP_OK ? werr : err;
}
