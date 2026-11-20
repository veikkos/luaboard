/*
 * Board bring-up and thin helpers over ESP-IDF drivers.
 * Everything board-specific goes through board.h.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "wake_time.h"

esp_err_t hw_init(void);

/* Which CoreS3 this is, detected at boot. Lite reads as "cores3" (same chips). */
const char *hw_model(void);                    /* "cores3" or "cores3se" */
const char *hw_model_name(void);               /* "CoreS3" or "CoreS3 SE" */

/* Serial link to the PC (USB-Serial/JTAG on the CoreS3). */
int  hw_serial_read(uint8_t *buf, size_t cap);          /* non-blocking */
bool hw_serial_write(const void *data, size_t len);  /* false: some was dropped */
void hw_serial_puts(const char *s);

/* ADC1. Channels are configured lazily on first read. raw: 0..4095 */
esp_err_t hw_adc_read(int channel, int *raw);
int  hw_adc_raw_to_mv(int raw);

/* GPIO with a guard list of pins the firmware itself uses. A script can
   take a pin the chip has (GPIO_IS_VALID_GPIO) that isn't reserved. */
bool hw_gpio_is_reserved(int pin);
/** Back to the state of a pin no app uses, the one hw_init() gives every
    free pin: ESP-IDF's reset state (not driven, the input off, the weak
    pull-up on). Forgets any ADC set-up. */
void hw_gpio_release(int pin);

/* Touch: true while a finger is down; coordinates in the 320x280 touch area
   (y >= LCD height = the button strip under the screen). */
bool hw_touch_read(int *x, int *y);

/* Speaker: 16-bit stereo frames (L, R) at SOUND_RATE. Start and stop from
   the main task (they share the internal I2C bus with everything else); a
   started output may be written from another thread. */
esp_err_t hw_audio_start(void);                /* power the amplifier, start the output */
/** Queue frames for the output, waiting while its buffers are full. */
esp_err_t hw_audio_write(const int16_t *frames, size_t n);
void hw_audio_stop(void);                      /* stop the output, amplifier off */

/* Power (AXP2101). */
esp_err_t hw_backlight(int percent);           /* 0 = off, 1..100 */
/** The screen off (backlight off whatever hw_backlight() says) or back on
 *  at the level last asked for. */
esp_err_t hw_screen_off(bool off);
bool hw_screen_is_off(void);
/** Let the chip go into light sleep whenever no task has work (true), or
 *  keep it awake (false, as from boot). The main loop decides (sleep.c). */
void hw_light_sleep_allow(bool on);
bool hw_battery_present(void);
int  hw_battery_percent(void);                 /* -1 if no battery or unknown */
bool hw_usb_powered(void);
bool hw_charging(void);
/** Side power key since the last call: 0 none, 1 short press, 2 long press. */
int  hw_power_key(void);

/* Real-time clock chip (BM8563): keeps time while the board has power (and,
   with a battery, while it's off). UTC seconds since 1970. */
/** False if there's no chip, or it lost power since it was set (time not to be trusted). */
bool hw_rtc_read(int64_t *utc);
esp_err_t hw_rtc_write(int64_t utc);
/* The chip's countdown timer or alarm, with its interrupt: what turns a
   powered-down board on again (hw_power_off). */
esp_err_t hw_rtc_wake_arm(const wake_plan_t *p);
/** Its timer or alarm went off (read at boot, before hw_rtc_wake_disarm). */
bool hw_rtc_wake_fired(void);
void hw_rtc_wake_disarm(void);

/** Power the board down. With `wake`, it turns itself on again by then;
 *  with NULL only the power key does (or plugging in USB power). Doesn't
 *  return. If the PMIC doesn't cut the power, the chip goes into deep sleep
 *  instead, which a touch (or the wake-up time) ends with a reset. */
void hw_power_off(const wake_plan_t *wake);
/** How the chip started: 'p' power applied (the power key, USB plugged in),
 *  't' a wake-up from deep sleep by its timer, 'r' any other reset. */
char hw_boot_kind(void);

/* The board's own sensors, on CoreS3 and Lite (the SE has none: every call
   is ESP_ERR_NOT_FOUND there). Both are off until started, and stopped
   again they draw next to nothing. */
/** The motion sensor (BMI270): out of reset, its configuration file loaded,
 *  accelerometer and gyroscope on. Takes about a third of a second. */
esp_err_t hw_imu_start(void);
void hw_imu_stop(void);
/** Raw readings: +-32768 is +-8 g, and +-2000 degrees a second. */
esp_err_t hw_imu_read(int16_t accel[3], int16_t gyro[3]);
/** The light and proximity sensor (LTR-553ALS): both parts measuring. Takes
 *  a bit over a tenth of a second (its first measurement). */
esp_err_t hw_light_start(void);
void hw_light_stop(void);
/** Its two light channels (visible + infrared, infrared) at gain 1 and
 *  100 ms, and the proximity reading (0..2047, more is nearer). */
esp_err_t hw_light_read(uint16_t *ch0, uint16_t *ch1, uint16_t *proximity);

/* 5V out to the Grove ports (A, B, C), on the IO expander. On at boot. */
void hw_port_power(bool on);
bool hw_port_power_on(void);

/* LCD reset line (on the IO expander). */
void hw_lcd_reset(void);

uint32_t hw_millis(void);   /* wraps after 49.7 days: compare differences only */
uint64_t hw_millis64(void); /* never wraps: what Lua sees */

/* A big buffer: in PSRAM where there is some, else internal RAM. free() it. */
void *hw_big_alloc(size_t n);

/* Hardware watchdog on the calling (main) task: reset the chip unless fed
   regularly. Stretch it around operations that legitimately take long
   (formatting the apps partition). */
void hw_watchdog_start(void);

/* The last reset was a crash: panic, a watchdog or a brown-out. */
bool hw_reset_was_crash(void);
/** Why the chip last reset: "poweron", "sw", "panic", "task-wdt", "brownout", ... */
const char *hw_reset_reason(void);
/* The last reset was the firmware's own fault: panic, a watchdog, a CPU
   lockup. Not a brown-out or power glitch, which bad power causes. */
bool hw_reset_was_fw_crash(void);
void hw_watchdog_feed(void);
/* Give the watchdog `ms` instead of its usual timeout, or with 0 the usual
   one again: for one long call that can't feed it. Still a watchdog, so a
   call that never returns still resets the chip. */
void hw_watchdog_stretch(uint32_t ms);
