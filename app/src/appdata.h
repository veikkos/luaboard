/* An app's data outside its files (secrets, store values, Bluetooth bonds),
   and removing an app with all of it. */
#pragma once

#include "esp_err.h"

/** Delete the app's data; the app stays. The secrets' result. */
esp_err_t appdata_clear(const char *app);

/** After the app's files are gone: its data, and autostart if it was this app. */
void appdata_forget(const char *app);

/** Uninstall an app: stop it if it runs, delete its files and appdata_forget()
 *  it. An app whose files are gone (an upload lost in a restart) but whose
 *  data is left counts as there. ESP_ERR_NOT_FOUND: no such app. */
esp_err_t app_remove(const char *app);

/** Every app the same way, and everything in RAM (FORMAT). Slow: it erases
 *  the whole apps partition. */
esp_err_t apps_wipe(void);
