/*
 * Firmware entry point. A single super-loop in the main FreeRTOS task:
 *   - poll the serial protocol (script upload / control from the PC)
 *   - tick the Lua script (timers + loop(dt))
 *   - home menu: touch buttons, leaving apps (hold B)
 *   - firmware updates: confirm a new firmware, reboot after an update,
 *     look for a newer release
 *   - yield one RTOS tick
 */
#include <string.h>

#include "app_config.h"
#include "app_update.h"
#include "appmeta.h"
#include "clock.h"
#include "cstack.h"
#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gfx.h"
#include "hw.h"
#include "auth.h"
#include "ble.h"
#include "lcd.h"
#include "net.h"
#include "ota.h"
#include "power.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "protocol.h"
#include "crash.h"
#include "screens.h"
#include "menu.h"
#include "script.h"
#include "settings.h"
#include "sleep.h"
#include "sound.h"
#include "update.h"
#include "vfs.h"

static const char *TAG = "main";

/* The USB-C serial link as a protocol link. */
static int usb_read(void *ctx, uint8_t *buf, size_t cap) { (void)ctx; return hw_serial_read(buf, cap); }
static void usb_write(void *ctx, const void *d, size_t n) { (void)ctx; hw_serial_write(d, n); }

/* Crash-loop guard: count boots that start the autostart app. The count is
   cleared when the app stops, when it has run for BOOT_STABLE_MS, and at a
   boot whose reset wasn't a crash (power-on, reset button, flashing). So it
   only grows while the device keeps crashing (panic, brown-out, watchdog
   reset) with the autostart app running, however long after its start; at
   BOOT_FAIL_LIMIT autostart is skipped. Lua errors don't reset the device,
   so they don't count. */
static uint8_t boot_fails_get(void)
{
    uint8_t v = 0;
    settings_load(NVS_KEY_BOOT_FAILS, &v, 1);
    return v;
}

static void boot_fails_set(uint8_t v) { settings_store(NVS_KEY_BOOT_FAILS, &v, 1); }

void app_main(void)
{
    cstack_init(CONFIG_ESP_MAIN_TASK_STACK_SIZE);   /* before any Lua */
    ESP_ERROR_CHECK(hw_init());

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS init failed (%s), erasing", esp_err_to_name(err));
        nvs_flash_erase();
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ota_boot();                        /* early: what crashes before this, the bootloader rolls back */
    /* From here a hang is a watchdog crash, which a new firmware's boot-loop
       count sees (ota.c), not a device stuck until someone pulls the power. */
    hw_watchdog_start();

    if (!gfx_init()) ESP_LOGE(TAG, "no memory for the framebuffer");
    if (lcd_init() != ESP_OK) ESP_LOGE(TAG, "LCD init failed - continuing headless (USB and Wi-Fi still work)");

    settings_init();
    clock_init();
    power_init();
    hw_backlight(settings_brightness());
    hw_watchdog_stretch(WATCHDOG_FORMAT_MS);   /* the first boot formats the apps partition: slow */
    vfs_init();
    hw_watchdog_stretch(0);
    auth_init();
    script_init();
    proto_init();
    proto_add_link(&(proto_link_t){
        .name = "usb", .trusted = true, .read = usb_read, .write = usb_write, .ctx = NULL,
    });
    net_init();
    update_init();
    app_update_init();
    proto_eventf("boot fw=%s ver=%s board=%s model=%s slot=%s ota=%s reset=%s boot=%s", FW_NAME, fw_version(),
                 BOARD_NAME, hw_model(), ota_slot(), ota_state(), hw_reset_reason(), power_boot_reason());

    /* A reset nobody asked for (a crash, a watchdog, a power dip) says so on
       the screen for a moment, so it isn't a silent restart. A touch goes on. */
    bool tapped_on = false;            /* the notice's tap isn't a finger held for safe mode */
    const crash_record_t *crash = crash_last();
    if (crash)
        proto_eventf("crash ver=%s cause=%s at=%s", crash->version, crash->cause,
                     crash->frames ? crash->frame[0] : "-");
    if (hw_reset_was_crash()) {
        screen_restarted(hw_reset_reason(), !hw_reset_was_fw_crash(), crash ? crash->cause : NULL,
                         crash && crash->frames ? crash->frame[0] : NULL);
        int x, y;
        bool was_down = hw_touch_read(&x, &y);           /* a finger held from boot (safe mode) doesn't end it */
        int shown = -1;
        bool updating = false;
        for (uint32_t t0 = hw_millis(); hw_millis() - t0 < RESTART_NOTICE_MS && !script_running();) {
            /* An update the PC sends meanwhile has the screen and reboots
               when it's in; one that fails has put up its own screen. */
            if (ota_active()) updating = true;
            else if (updating) break;
            int left = (int)((RESTART_NOTICE_MS - (hw_millis() - t0) + 999) / 1000);   /* 10, 9, ... 1 */
            if (!updating && left != shown) screen_restarted_countdown(shown = left);
            hw_watchdog_feed();
            proto_poll();                                /* the PC isn't kept waiting meanwhile */
            net_poll();
            ota_poll();
            vTaskDelay(1);
            bool down = hw_touch_read(&x, &y);
            if (down && !was_down) { tapped_on = true; break; }
            was_down = was_down && down;
        }
    }

    /* An app the PC started during the notice keeps running: no autostart
       in its place, no menu over it. */
    bool pc_started = script_running();

    /* Safe mode: keep a finger on the screen during boot to skip autostart. */
    int tx, ty;
    bool safe_mode = !tapped_on && hw_touch_read(&tx, &ty);
    menu_init();                       /* (ignores that finger until it's lifted) */

    /* The app the device turned itself off under comes first, once: it is
       started like the autostart app (safe mode and the crash-loop guard
       apply), and falls through to the menu if it's gone. */
    char resume[VFS_APP_NAME_MAX];
    const char *autostart = power_resume_app(resume, sizeof resume) ? resume : settings_autostart();
    char start_path[VFS_MAX_NAME];
    snprintf(start_path, sizeof start_path, "%s/main.lua", autostart);
    bool have_start = autostart[0] && vfs_exists(start_path);
    /* One this firmware won't run (its lua_api out of range, after an
       update) stays installed and keeps its setting, for a firmware that
       runs it again: the menu instead of its error at every boot. */
    bool api_bad = false;
    if (have_start) {
        app_meta_t meta;
        char why[80];
        app_meta_load(autostart, &meta);
        api_bad = app_meta_api_problem(&meta, why, sizeof why) != NULL;
    }

    uint8_t fails = boot_fails_get();
    if (fails && !hw_reset_was_crash()) { fails = 0; boot_fails_set(0); }
    bool guard_armed = false;
    uint32_t script_started = 0;

    if (pc_started) {
        /* (nothing to start; a crash-loop count stays as it was) */
    } else if (have_start && safe_mode) {
        menu_show();
        menu_toast("safe mode: autostart skipped");
        proto_eventf("safe-mode reason=touch");
    } else if (have_start && fails >= BOOT_FAIL_LIMIT) {
        menu_show();
        menu_toast("crashed 3 times: autostart skipped");
        proto_eventf("safe-mode reason=crash-loop boots=%d", fails);
        boot_fails_set(0);   /* next boot tries again */
    } else if (api_bad) {
        menu_show();
        menu_toast("autostart app won't run on this firmware");
        proto_eventf("safe-mode reason=lua_api");
    } else if (have_start) {
        char msg[200];
        boot_fails_set((uint8_t)(fails + 1));
        guard_armed = true;
        script_started = hw_millis();
        if (script_run(autostart, msg, sizeof msg)) proto_eventf("started %s", script_current());
        else proto_eventf("error %s", msg);      /* the error screen is already up */
    } else {
        menu_show();
        if (safe_mode) proto_eventf("safe-mode reason=touch");
    }

    if (!strcmp(ota_state(), "rolledback")) {
        proto_eventf("ota rolled-back ver=%s", fw_version());
        if (!script_running()) menu_toast("the update kept crashing: old firmware back");
    } else if (ota_just_updated() && !script_running()) {
        char msg[64];
        snprintf(msg, sizeof msg, "updated to %s", fw_version());
        menu_toast(msg);
    }

    for (;;) {
        hw_watchdog_feed();
        proto_poll();
        net_poll();
        ble_poll();
        clock_poll();
        ota_poll();
        update_poll();                  /* (starts no check while an update runs) */
        app_update_poll();
        sound_poll();
        /* An update has the screen until it reboots; a device that acts
           off (on USB power) runs nothing until it's turned on. */
        if (!ota_active() && !power_acting_off()) {
            script_tick();
            menu_tick();
        }
        vfs_data_poll();
        power_poll();                   /* sys.off: turns the device off (and doesn't return) */

        /* Stopped (left, error) or ran long enough: a later crash isn't a loop. */
        if (guard_armed && (!script_running() || hw_millis() - script_started >= BOOT_STABLE_MS)) {
            boot_fails_set(0);
            guard_armed = false;
        }
        /* Asleep on battery the chip may go into light sleep, which it only
           does while this task waits: for the script's next tick, or
           SLEEP_POLL_MS for a touch or the power key. */
        bool light = sleep_light_allowed();
        hw_light_sleep_allow(light);
        uint32_t wait_ms = 0;
        if (light) {
            wait_ms = script_next_due_ms();
            if (wait_ms > SLEEP_POLL_MS) wait_ms = SLEEP_POLL_MS;
        }
        TickType_t ticks = (TickType_t)((wait_ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS);
        vTaskDelay(ticks ? ticks : 1);
    }
}
