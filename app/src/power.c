#include "power.h"
#include "app_update.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "buttons.h"
#include "esp_log.h"
#include "hw.h"
#include "menu.h"
#include "ota.h"
#include "protocol.h"
#include "script.h"
#include "settings.h"
#include "sleep.h"

static const char *TAG = "power";

static const char *s_boot = "reset";

static struct {
    bool pending;
    bool timed;                  /* plan is set: it turns itself on again */
    wake_plan_t plan;
    char by[8];
} s_req;

/* Off acted out on USB power (power.h). */
static struct {
    bool on;
    bool timed;
    uint32_t until;              /* hw_millis() of the wake-up time */
    char script[96];             /* "<app>/<file>" to start again, "" if none ran */
    char app[64];
    uint32_t polled;             /* the power key and USB power: read every ACT_POLL_MS */
} s_act;

#define ACT_POLL_MS 50

bool power_acting_off(void) { return s_act.on; }

static bool s_keep_on;           /* an app holds off the Turn off after setting */
static uint32_t s_idle_checked;

void power_keep_on(bool on) { s_keep_on = on; }
bool power_kept_on(void) { return s_keep_on; }

/* The Turn off after setting: asleep for its minutes, on battery, and no
   app keeping the device on. Looked at once a second. */
static void idle_poll(void)
{
    uint32_t now = hw_millis();
    if (now - s_idle_checked < 1000) return;
    s_idle_checked = now;
    int min = settings_turn_off();
    if (min <= 0 || s_keep_on || !sleep_active() || now - sleep_since() < (uint32_t)min * 60000u) return;
    if (hw_usb_powered()) return;
    power_off_request(0, "setting");
}

void power_init(void)
{
    char kind = hw_boot_kind();
    s_boot = kind == 'r' ? "reset" : (kind == 't' || hw_rtc_wake_fired()) ? "timer" : "power";
    hw_rtc_wake_disarm();
}

const char *power_boot_reason(void) { return s_boot; }

/* The app to start again at the next boot ("" = none). */
static void resume_note(const char *app) { settings_store(NVS_KEY_RESUME, app, *app ? strlen(app) + 1 : 0); }

bool power_resume_app(char *app, size_t cap)
{
    size_t len = settings_take(NVS_KEY_RESUME, app, cap);   /* once: a boot after this one goes the normal way */
    return len > 1 && !app[len - 1];
}

wake_err_t power_off_request(uint32_t seconds, const char *by)
{
    wake_plan_t plan = { 0 };
    if (seconds) {
        /* The alarm is set in the clock chip's own time. */
        int64_t now = 0;
        bool known = hw_rtc_read(&now);
        wake_err_t err = wake_plan(seconds, known, now, &plan);
        if (err != WAKE_OK) return err;
    }
    s_req.pending = true;
    s_req.timed = seconds != 0;
    s_req.plan = plan;
    snprintf(s_req.by, sizeof s_req.by, "%s", by);
    return WAKE_OK;
}

/* The acted-out off ends as a boot would: the screen on, and the script
   that ran again from its start. */
static void act_end(const char *boot)
{
    s_act.on = false;
    s_boot = boot;
    sleep_activity(hw_millis());
    if (!s_act.script[0]) return;
    /* Safe mode, as at a boot: a finger held on the screen as the key turns
       the device on skips the app. Without it, an app that turns the device
       off as it starts could only be left by unplugging USB. */
    int x, y;
    if (!strcmp(boot, "power") && hw_touch_read(&x, &y)) {
        buttons_reset();                         /* that finger isn't a tap on the menu */
        menu_show();
        menu_toast("safe mode: app skipped");
        proto_eventf("safe-mode reason=touch");
        return;
    }
    char err[200];
    if (script_run(s_act.script, err, sizeof err)) proto_eventf("started %s", script_current());
    else proto_eventf("error %s", err);          /* the error screen is up */
}

static void act_poll(void)
{
    uint32_t now = hw_millis();
    if (script_running()) {                      /* started from the PC meanwhile: on again, nothing else */
        s_act.on = false;
        sleep_activity(now);
        return;
    }
    if (s_act.timed && (int32_t)(now - s_act.until) >= 0) { act_end("timer"); return; }
    /* The screen was woken some other way (the power key pressed from the
       PC, disp.sleep(false) by EVAL): that is the device turned on. */
    if (!sleep_active()) { act_end("power"); return; }
    if (now - s_act.polled < ACT_POLL_MS) return;
    s_act.polled = now;
    if (hw_power_key()) { act_end("power"); return; }
    if (hw_usb_powered()) return;
    /* USB was unplugged: off for real, for what's left of the time. A rest
       too short to power down for is waited out here. */
    wake_plan_t plan;
    if (s_act.timed) {
        int64_t rtc = 0;
        bool known = hw_rtc_read(&rtc);
        uint32_t left = (s_act.until - now) / 1000 + OFF_BOOT_ALLOWANCE_S;
        if (wake_plan(left, known, rtc, &plan) != WAKE_OK) return;
    }
    resume_note(s_act.app);
    hw_power_off(s_act.timed ? &plan : NULL);
}

void power_poll(void)
{
    /* A firmware update has the device until it reboots or fails, an app
       install until it's done: nothing turns the device off under them, and
       an acted-out off doesn't end (and start the app) over them. What was
       asked for waits. */
    if (ota_active() || app_install_busy()) return;
    if (s_act.on) { act_poll(); return; }
    if (!s_req.pending) idle_poll();
    if (!s_req.pending) return;
    s_req.pending = false;
    char app[64] = "", cur[96] = "";
    if (script_running()) {
        snprintf(cur, sizeof cur, "%s", script_current());
        script_app(app, sizeof app);
        script_stop();               /* as sys.exit() ends it: store written, connections closed */
        proto_eventf("stopped %s (off)", cur);
    }
    char wake[16] = "none";
    if (s_req.timed) snprintf(wake, sizeof wake, "%u", (unsigned)s_req.plan.after_s);
    proto_eventf("off wake=%s by=%s", wake, s_req.by);
    ESP_LOGI(TAG, "off wake=%s by=%s", wake, s_req.by);
    if (hw_usb_powered()) {
        s_act.on = true;
        s_act.timed = s_req.timed;
        s_act.until = hw_millis() + s_req.plan.after_s * 1000u;
        s_act.polled = hw_millis();
        snprintf(s_act.script, sizeof s_act.script, "%s", cur);
        snprintf(s_act.app, sizeof s_act.app, "%s", app);
        hw_power_key();              /* a press from before isn't the one that turns it on */
        sleep_now();
        return;
    }
    resume_note(app);                /* the next boot starts it again (or, from the menu, nothing) */
    hw_power_off(s_req.timed ? &s_req.plan : NULL);
}
