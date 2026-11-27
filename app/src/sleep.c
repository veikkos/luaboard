#include "sleep.h"
#include "app_update.h"

#include "app_config.h"
#include "ble.h"
#include "gfx.h"
#include "hw.h"
#include "lua_api.h"
#include "lua_uart.h"
#include "ota.h"
#include "protocol.h"
#include "settings.h"
#include "sound.h"

static uint32_t s_active_at;    /* what Sleep after counts from: the last touch or key, or an app letting the screen go */
static uint32_t s_touched_at;   /* the last touch or key (the start, before any): what Turn off after counts from */
static bool s_usb;              /* USB power, as last read */
static uint32_t s_usb_at;
static bool s_usb_read;
static bool s_started;
static bool s_keep;              /* an app keeps the screen on */
static bool s_awake;             /* a computer keeps the device awake (sleep_stay_awake) ... */
static uint32_t s_awake_until;   /* ... until then */

void sleep_now(void)
{
    if (hw_screen_is_off()) return;
    hw_screen_off(true);                /* the light first: the panel going to sleep isn't seen */
    gfx_sleep(true);
    proto_eventf("screen off");
}

uint32_t sleep_idle_since(void) { return s_touched_at; }

bool sleep_light_allowed(void)
{
    if (!POWER_LIGHT_SLEEP || !hw_screen_is_off()) return false;
    uint32_t now = hw_millis();
    if (!s_usb_read || now - s_usb_at >= 1000) {       /* a register read over I2C: not on every pass */
        s_usb = hw_usb_powered();
        s_usb_at = now;
        s_usb_read = true;
    }
    return !s_usb && !sound_playing() && !ota_active() && !app_install_busy() && !ble_busy() && !lua_uart_is_open() &&
           !lua_api_pwm_running() && !hw_camera_on() && !sleep_stay_awake_left(now);
}

bool sleep_active(void) { return hw_screen_is_off(); }

bool sleep_activity(uint32_t now)
{
    s_active_at = s_touched_at = now;
    s_started = true;
    if (!hw_screen_is_off()) return false;
    gfx_sleep(false);                   /* the panel first: the light comes on over its picture */
    hw_screen_off(false);
    proto_eventf("screen on");
    return true;
}

void sleep_keep_awake(bool on) { s_keep = on; }

bool sleep_kept_awake(void) { return s_keep; }

void sleep_stay_awake(uint32_t now, uint32_t ms)
{
    s_awake = ms > 0;
    s_awake_until = now + ms;
    if (s_awake) sleep_activity(now);
}

uint32_t sleep_stay_awake_left(uint32_t now)
{
    if (s_awake && (int32_t)(s_awake_until - now) <= 0) s_awake = false;
    return s_awake ? s_awake_until - now : 0;
}

void sleep_tick(uint32_t now)
{
    if (sleep_stay_awake_left(now)) s_active_at = s_touched_at = now;   /* as if touched: the settings count from its end */
    if (!s_started) s_touched_at = now;
    if (!s_started || s_keep) { s_active_at = now; s_started = true; }   /* kept: idle from when it's let go */
    int min = settings_screen_off();
    if (min > 0 && !hw_screen_is_off() && now - s_active_at >= (uint32_t)min * 60000u) sleep_now();
}
