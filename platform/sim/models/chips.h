/*
 * Register-level models of the small I2C chips on M5Stack-style boards.
 * Only the registers the firmware uses are given meaning; everything else
 * behaves as plain read/write storage.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "i2c_regs.h"

/* ---- AXP2101 PMIC ------------------------------------------------------
 *   0x00 status1   bit5 = VBUS good, bit3 = battery present
 *   0x01 status2   bits6:5 = 01 charging, 10 discharging, 00 standby
 *   0x90 LDO en 0  bit7 = DLDO1 enable
 *   0x41 IRQ en 1  bits3:2 = PWRON short/long press
 *   0x49 IRQ st 1  bit3 = PWRON short press, bit2 = long press; write 1 to clear
 *   0x99 DLDO1 V   bits4:0, V = 0.5 V + 0.1 V * n  (backlight supply)
 *   0xA4 battery percentage 0..100                                      */
typedef struct axp2101 axp2101_t;
struct axp2101 {
    i2c_regs_t r;
    uint8_t irq1;
    void (*on_power_off)(axp2101_t *m);   /* 0x10 bit 0 written: the board's power is cut */
};
void axp2101_init(axp2101_t *m);
/** Backlight level 0..1 derived from DLDO1 enable + voltage. */
float axp2101_backlight(axp2101_t *m);
void axp2101_set_battery(axp2101_t *m, int percent, bool usb_power);   /* percent < 0: no battery */
/** The power key was pressed (long = held past the PMIC's long-press time). */
void axp2101_power_key(axp2101_t *m, bool long_press);

/* ---- AW9523B GPIO expander ---------------------------------------------
 *   0x02/0x03 output P0/P1, 0x04/0x05 config P0/P1 (1 = input)
 *   0x10 ID (0x23)                                                       */
typedef struct aw9523 aw9523_t;
struct aw9523 {
    i2c_regs_t r;
    /* Called when an output pin changes: port 0/1, bit, new level. */
    void (*on_pin)(aw9523_t *m, int port, int bit, int level);
    void *user;
};
void aw9523_init(aw9523_t *m);

/* ---- AW88298 speaker amplifier (I2S in) ---------------------------------
 *   16-bit registers, high byte first: a write is [reg, hi, lo, hi, lo, ...]
 *   (the register pointer goes up by one per word), a read continues from
 *   the pointer. Held in reset (its reset line low) it forgets everything.
 *   0x00 ID 0x1852
 *   0x04 SYSCTRL  bit0 PWDN, bit1 AMPPD (1 = powered down), bit6 I2SEN
 *   0x06 I2SCTRL  bits3:0 I2SSR, the I2S rate: 8000, 11025, 12000, 16000,
 *                 22050, 24000, 32000, 44100, 48000, 96000 Hz
 *   The volume (0x0C) isn't modelled: it plays the samples as they come.  */
typedef struct {
    pthread_mutex_t lock;
    uint16_t reg[256];
    uint8_t ptr;
    bool in_reset;
    bool warned;              /* said once that the rate doesn't match */
} aw88298_t;
void aw88298_init(aw88298_t *m);
void aw88298_write(void *ctx, const uint8_t *data, size_t len);
void aw88298_read(void *ctx, uint8_t *buf, size_t len);
/** Its reset line: true while held low. */
void aw88298_set_reset(aw88298_t *m, bool in_reset);
/** Would it play I2S frames coming at `rate` now: out of reset, powered
 *  up, I2S on and set to that rate? */
bool aw88298_playing(aw88298_t *m, uint32_t rate);

/* ---- FT6336U capacitive touch ------------------------------------------
 *   0x02 TD_STATUS: number of points (0/1)
 *   0x03 P1_XH (bits3:0 X high, bits7:6 event), 0x04 P1_XL
 *   0x05 P1_YH (bits3:0 Y high),                0x06 P1_YL
 *   0xA8 vendor ID 0x11                                                  */
typedef struct { i2c_regs_t r; bool in_reset; unsigned reads; } ft6336u_t;
void ft6336u_init(ft6336u_t *m);
/** How many times the firmware has read TD_STATUS: looked for a finger. */
unsigned ft6336u_reads(ft6336u_t *m);
/** Report a finger at (x, y), or no finger with down = false. */
void ft6336u_set_touch(ft6336u_t *m, bool down, int x, int y);

/* ---- BM8563 real-time clock (PCF8563 layout) ---------------------------
 *   0x02 seconds (bit7 VL: lost power), 0x03 min, 0x04 hour, 0x05 day,
 *   0x06 weekday, 0x07 month (bit7 century), 0x08 year 00..99; BCD.
 *   Runs on the host clock plus an offset; reading 0x02 latches the time.
 *   `on_set` is called when the firmware has written a complete time.
 *   0x01 control 2: bit3 AF, bit2 TF (written 0 to clear), bit1 AIE, bit0 TIE.
 *   0x09 minute, 0x0A hour, 0x0B day, 0x0C weekday alarm (bit7: not compared).
 *   0x0E timer control: bit7 enable, bits1:0 source (2 = 1 Hz, 3 = 1/60 Hz);
 *   0x0F its count. The timer and the alarm run on the world's clock; the
 *   interrupt output (bm8563_irq) is what turns a powered-down board on.
 *   `on_wake_set` is called when either was armed or changed.            */
typedef struct bm8563 bm8563_t;
struct bm8563 {
    i2c_regs_t r;
    long long offset;        /* chip time - host time, seconds */
    bool valid;              /* VL clear: the time can be trusted */
    void (*on_set)(bm8563_t *m);
    void (*on_wake_set)(bm8563_t *m);
    uint8_t ctl2;            /* control 2 as it stands (a write can't set AF or TF) */
    long long timer_at;      /* world seconds when the timer runs out, 0 = not running */
    long long alarm_at;      /* ... when the alarm next matches, 0 = not set */
    void *user;
};
void bm8563_init(bm8563_t *m);
/** Set the chip's time (UTC seconds) and validity, e.g. restored from a file. */
void bm8563_set(bm8563_t *m, long long utc, bool valid);
/** Power lost: the time is no longer reliable (VL set). */
void bm8563_lose_power(bm8563_t *m);
/** The interrupt output is active: the timer ran out or the alarm matched,
 *  with its interrupt enabled. */
bool bm8563_irq(bm8563_t *m);

/* ---- SHT40 temperature and humidity sensor (e.g. M5Stack ENV IV unit) ---
 *   Not register based: a write is a one-byte command, the next read gets
 *   its answer. 0xFD/0xF6/0xE0 measure (high/medium/low precision): 6 bytes,
 *   T msb lsb crc, RH msb lsb crc; 0x89 serial number: 6 bytes; 0x94 soft
 *   reset. CRC-8 poly 0x31, init 0xFF. T = -45 + 175 * raw / 65535 °C,
 *   RH = -6 + 125 * raw / 65535 %. Address 0x44.                         */
typedef struct {
    float temp_c, humidity;
    uint8_t cmd;              /* last command; 0 = none (a read NACKs on the chip, 0xFF here) */
} sht40_t;
void sht40_init(sht40_t *m);
void sht40_write(void *ctx, const uint8_t *data, size_t len);
void sht40_read(void *ctx, uint8_t *buf, size_t len);

/* ---- BMI270 motion sensor (accelerometer and gyroscope) -------------------
 *   0x00 chip ID 0x24. After a soft reset (0x7E = 0xB6) it needs its 8192
 *   byte configuration file written to 0x5E (which doesn't count on: every
 *   byte of a burst goes there), then 0x59 = 1: 0x21 internal status reads
 *   1 if the whole file came, else 2. With 0x7D bits 2 and 1 set, 0x0C..0x11
 *   are the acceleration and 0x12..0x17 the rotation, 16-bit little-endian,
 *   at +-8 g and +-2000 degrees a second full scale.                      */
typedef struct {
    pthread_mutex_t lock;
    uint8_t reg[128];
    uint8_t ptr;
    size_t config_bytes;     /* written to 0x5E since the reset */
    bool ready;              /* the configuration file was taken */
    float accel[3];          /* g */
    float gyro[3];           /* degrees a second */
} bmi270_t;
void bmi270_init(bmi270_t *m);
void bmi270_write(void *ctx, const uint8_t *data, size_t len);   /* sim_i2c_write_fn */
void bmi270_read(void *ctx, uint8_t *buf, size_t len);           /* sim_i2c_read_fn */
void bmi270_set(bmi270_t *m, const float accel[3], const float gyro[3]);   /* NULL: as it is */
/** Measuring: configured, with the accelerometer or the gyroscope on. */
bool bmi270_active(bmi270_t *m);

/* ---- LTR-553ALS ambient light and proximity sensor -------------------------
 *   0x80 ALS control (bit0 active), 0x81 PS control (bits1:0 = 3 active),
 *   0x86 part ID 0x92, 0x87 manufacturer 0x05, 0x88..0x8B the light
 *   channels 1 and 0 (16-bit little-endian), 0x8D/0x8E proximity (11 bits).
 *   Channel 0 follows `lux`, channel 1 stays 0 (the visible light of a
 *   white lamp, roughly); a part that isn't active reads 0.               */
typedef struct { i2c_regs_t r; float lux; int proximity; } ltr553_t;
void ltr553_init(ltr553_t *m);
void ltr553_set(ltr553_t *m, float lux, int proximity);   /* a negative value: as it is */
bool ltr553_active(ltr553_t *m);

