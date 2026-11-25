#include "chips.h"
#include "sim_hw.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------- AXP2101 */

/* IRQ status bits are write-1-to-clear. */
static void axp_write(i2c_regs_t *r, uint8_t addr, uint8_t v)
{
    axp2101_t *m = r->user;
    if (addr == 0x49) {
        m->irq1 &= (uint8_t)~v;
        r->reg[0x49] = m->irq1;
    }
    if (addr == 0x10 && (v & 0x01) && m->on_power_off) m->on_power_off(m);
}

void axp2101_power_key(axp2101_t *m, bool long_press)
{
    pthread_mutex_lock(&m->r.lock);
    m->irq1 |= long_press ? 0x04 : 0x08;
    m->r.reg[0x49] = m->irq1;
    pthread_mutex_unlock(&m->r.lock);
}

void axp2101_init(axp2101_t *m)
{
    i2c_regs_init(&m->r);
    m->r.user = m;
    m->r.on_write = axp_write;
    m->on_power_off = NULL;
    m->irq1 = 0;
    m->r.reg[0x03] = 0x4A;   /* chip ID */
    axp2101_set_battery(m, 80, true);
}

float axp2101_backlight(axp2101_t *m)
{
    uint8_t en = i2c_regs_get(&m->r, 0x90);
    uint8_t n = i2c_regs_get(&m->r, 0x99) & 0x1F;
    if (!(en & 0x80)) return 0.0f;
    float v = 0.5f + 0.1f * (float)n;
    /* LED string barely lights below ~2.4 V, full brightness at 3.3 V.
       sqrt: perceived brightness rises fast at the low end. */
    float k = (v - 2.4f) / 0.9f;
    return k <= 0 ? 0 : k >= 1 ? 1 : sqrtf(k);
}

void axp2101_set_battery(axp2101_t *m, int percent, bool usb)
{
    bool present = percent >= 0;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    i2c_regs_set(&m->r, 0xA4, present ? (uint8_t)percent : 0);
    i2c_regs_set(&m->r, 0x00, (usb ? 0x20 : 0x00) | (present ? 0x08 : 0x00));
    uint8_t chg = !present ? 0x00 : usb ? (percent < 100 ? 0x20 : 0x00) : 0x40;
    i2c_regs_set(&m->r, 0x01, chg);
}

/* ----------------------------------------------------------------- AW9523 */

static void aw_write(i2c_regs_t *r, uint8_t addr, uint8_t v)
{
    aw9523_t *m = r->user;
    if ((addr == 0x02 || addr == 0x03) && m->on_pin) {
        /* r->reg already holds the new value; compare against shadow in 0xF0/0xF1 */
        int port = addr - 0x02;
        uint8_t old = r->reg[0xF0 + port];
        r->reg[0xF0 + port] = v;
        for (int b = 0; b < 8; b++)
            if (((old ^ v) >> b) & 1) m->on_pin(m, port, b, (v >> b) & 1);
    }
    if ((addr == 0x04 || addr == 0x05) && m->on_config) m->on_config(m);
}

void aw9523_init(aw9523_t *m)
{
    i2c_regs_init(&m->r);
    m->r.user = m;
    m->r.on_write = aw_write;
    m->r.reg[0x10] = 0x23;   /* ID */
    m->on_pin = 0;
    m->on_config = 0;
}

/* ---------------------------------------------------------------- AW88298 */

static void aw88_defaults(aw88298_t *m)
{
    memset(m->reg, 0, sizeof m->reg);
    m->reg[0x00] = 0x1852;
    m->reg[0x04] = 0x4003;    /* powered down, I2S off */
    m->ptr = 0;
}

void aw88298_init(aw88298_t *m)
{
    pthread_mutex_init(&m->lock, NULL);
    aw88_defaults(m);
    m->in_reset = true;       /* until its reset line goes high */
    m->warned = false;
}

void aw88298_set_reset(aw88298_t *m, bool in_reset)
{
    pthread_mutex_lock(&m->lock);
    if (in_reset) aw88_defaults(m);
    m->in_reset = in_reset;
    pthread_mutex_unlock(&m->lock);
}

void aw88298_write(void *ctx, const uint8_t *data, size_t len)
{
    aw88298_t *m = ctx;
    pthread_mutex_lock(&m->lock);
    if (!m->in_reset && len) {
        m->ptr = data[0];
        for (size_t i = 1; i + 1 < len; i += 2) {
            if (m->ptr != 0x00) m->reg[m->ptr] = (uint16_t)(data[i] << 8 | data[i + 1]);   /* the ID is read-only */
            m->ptr++;
        }
    }
    pthread_mutex_unlock(&m->lock);
}

void aw88298_read(void *ctx, uint8_t *buf, size_t len)
{
    aw88298_t *m = ctx;
    pthread_mutex_lock(&m->lock);
    for (size_t i = 0; i < len; i += 2) {
        uint16_t v = m->in_reset ? 0xFFFF : m->reg[m->ptr++];
        buf[i] = (uint8_t)(v >> 8);
        if (i + 1 < len) buf[i + 1] = (uint8_t)v;
    }
    pthread_mutex_unlock(&m->lock);
}

bool aw88298_playing(aw88298_t *m, uint32_t rate)
{
    static const uint32_t rates[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 96000 };
    pthread_mutex_lock(&m->lock);
    uint16_t sys = m->reg[0x04], sr = m->reg[0x06] & 0x0F;
    bool on = !m->in_reset && !(sys & 0x03) && (sys & 0x40);
    bool rate_ok = sr < sizeof rates / sizeof *rates && rates[sr] == rate;
    if (on && !rate_ok && !m->warned) {
        m->warned = true;
        fprintf(stderr, "AW88298: I2S comes at %u Hz but I2SSR is %u: the speaker stays silent\n", (unsigned)rate, sr);
    }
    pthread_mutex_unlock(&m->lock);
    return on && rate_ok;
}

/* ---------------------------------------------------------------- FT6336U */

void ft6336u_init(ft6336u_t *m)
{
    i2c_regs_init(&m->r);
    m->r.reg[0xA8] = 0x11;   /* FocalTech vendor ID */
    m->r.reg[0xA3] = 0x64;   /* chip ID */
    m->r.reg[0xA6] = 0x10;   /* firmware ID */
    m->in_reset = false;
}

void ft6336u_set_firmware(ft6336u_t *m, uint8_t id) { i2c_regs_set(&m->r, 0xA6, id); }

void ft6336u_set_touch(ft6336u_t *m, bool down, int x, int y)
{
    if (m->in_reset) down = false;
    pthread_mutex_lock(&m->r.lock);
    m->r.reg[0x02] = down ? 1 : 0;
    m->r.reg[0x03] = (uint8_t)((down ? 0x80 : 0x40) | ((x >> 8) & 0x0F)); /* contact / lift-up */
    m->r.reg[0x04] = (uint8_t)(x & 0xFF);
    m->r.reg[0x05] = (uint8_t)((y >> 8) & 0x0F);
    m->r.reg[0x06] = (uint8_t)(y & 0xFF);
    pthread_mutex_unlock(&m->r.lock);
}

/* ----------------------------------------------------------------- BM8563 */

/* The RTC chip counts the world's time (the simulator's, not the Mac's). */
static long long world_s(void) { return (long long)(sim_world_us() / 1000000); }

static uint8_t to_bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static int from_bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

static void bm_latch(bm8563_t *m)
{
    time_t t = (time_t)(world_s() + m->offset);
    struct tm tm;
    gmtime_r(&t, &tm);
    uint8_t *r = m->r.reg;
    r[0x02] = (uint8_t)(to_bcd(tm.tm_sec) | (m->valid ? 0 : 0x80));
    r[0x03] = to_bcd(tm.tm_min);
    r[0x04] = to_bcd(tm.tm_hour);
    r[0x05] = to_bcd(tm.tm_mday);
    r[0x06] = (uint8_t)tm.tm_wday;
    r[0x07] = to_bcd(tm.tm_mon + 1);
    r[0x08] = to_bcd((tm.tm_year + 1900) % 100);
}

/* AF and TF as they are now (the registers' lock held). */
static void bm_flags(bm8563_t *m)
{
    long long now = world_s();
    if (m->timer_at && now >= m->timer_at) { m->ctl2 |= 0x04; m->timer_at = 0; }
    if (m->alarm_at && now >= m->alarm_at) { m->ctl2 |= 0x08; m->alarm_at = 0; }
    m->r.reg[0x01] = m->ctl2;
}

/* The next whole minute after now (chip time) that the alarm registers
   match, as world seconds; 0 if no field is compared. */
static long long bm_alarm_next(bm8563_t *m)
{
    const uint8_t *g = m->r.reg;
    if ((g[0x09] & g[0x0A] & g[0x0B] & g[0x0C] & 0x80)) return 0;
    long long t = world_s() + m->offset;
    t = t - t % 60 + 60;
    for (int i = 0; i < 32 * 24 * 60; i++, t += 60) {
        time_t tt = (time_t)t;
        struct tm tm;
        gmtime_r(&tt, &tm);
        if (!(g[0x09] & 0x80) && from_bcd(g[0x09] & 0x7F) != tm.tm_min) continue;
        if (!(g[0x0A] & 0x80) && from_bcd(g[0x0A] & 0x3F) != tm.tm_hour) continue;
        if (!(g[0x0B] & 0x80) && from_bcd(g[0x0B] & 0x3F) != tm.tm_mday) continue;
        if (!(g[0x0C] & 0x80) && (g[0x0C] & 0x07) != tm.tm_wday) continue;
        return t - m->offset;
    }
    return 0;
}

static void bm_read(i2c_regs_t *r, uint8_t addr)
{
    if (addr == 0x02) bm_latch(r->user);
    if (addr == 0x01) bm_flags(r->user);
}

static void bm_write(i2c_regs_t *r, uint8_t addr, uint8_t v)
{
    bm8563_t *m = r->user;
    if (addr == 0x01) {                            /* AF and TF: 0 clears, 1 leaves them as they are */
        bm_flags(m);
        m->ctl2 = (uint8_t)((v & 0x13) | (m->ctl2 & 0x0C & v));
        r->reg[0x01] = m->ctl2;
        if (m->on_wake_set) m->on_wake_set(m);
    } else if (addr >= 0x09 && addr <= 0x0C) {
        m->alarm_at = bm_alarm_next(m);
        if (m->on_wake_set) m->on_wake_set(m);
    } else if (addr == 0x0E || addr == 0x0F) {
        /* Enabled with a 1 Hz or a 1/60 Hz source: it runs out `count`
           periods from now (the faster sources: at once, to the second). */
        uint8_t ctl = r->reg[0x0E], n = r->reg[0x0F];
        int period = (ctl & 3) == 3 ? 60 : (ctl & 3) == 2 ? 1 : 0;
        m->timer_at = (ctl & 0x80) && n ? world_s() + (long long)n * period + (period ? 0 : 1) : 0;
        if (m->on_wake_set) m->on_wake_set(m);
    }
    if (addr != 0x08) return;                      /* a complete time ends with the year */
    uint8_t *g = r->reg;
    struct tm tm = { .tm_year = 100 + from_bcd(g[0x08]), .tm_mon = from_bcd(g[0x07] & 0x1F) - 1,
                     .tm_mday = from_bcd(g[0x05] & 0x3F), .tm_hour = from_bcd(g[0x04] & 0x3F),
                     .tm_min = from_bcd(g[0x03] & 0x7F), .tm_sec = from_bcd(g[0x02] & 0x7F) };
    long long t = (long long)timegm(&tm);
    m->offset = t - world_s();
    m->valid = !(g[0x02] & 0x80);
    if (m->on_set) m->on_set(m);
}

void bm8563_init(bm8563_t *m)
{
    i2c_regs_init(&m->r);
    m->r.user = m;
    m->r.on_read = bm_read;
    m->r.on_write = bm_write;
    m->on_set = NULL;
    m->on_wake_set = NULL;
    m->timer_at = m->alarm_at = 0;
    m->ctl2 = 0;
    m->user = NULL;
    m->r.reg[0x09] = m->r.reg[0x0A] = m->r.reg[0x0B] = m->r.reg[0x0C] = 0x80;   /* no alarm */
    bm8563_lose_power(m);
}

bool bm8563_irq(bm8563_t *m)
{
    pthread_mutex_lock(&m->r.lock);
    bm_flags(m);
    uint8_t c = m->r.reg[0x01];
    pthread_mutex_unlock(&m->r.lock);
    return ((c & 0x08) && (c & 0x02)) || ((c & 0x04) && (c & 0x01));
}

void bm8563_set(bm8563_t *m, long long utc, bool valid)
{
    m->offset = utc - world_s();
    m->valid = valid;
}

void bm8563_lose_power(bm8563_t *m)
{
    bm8563_set(m, 946684800, false);               /* 2000-01-01, VL set */
}

/* ---------------------------------------------------------------- SHT40 */

static uint8_t sht_crc(const uint8_t *d, int n)
{
    uint8_t c = 0xFF;
    for (int i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++) c = (uint8_t)(c & 0x80 ? (c << 1) ^ 0x31 : c << 1);
    }
    return c;
}

static void sht_word(uint8_t *out, unsigned v)
{
    out[0] = (uint8_t)(v >> 8);
    out[1] = (uint8_t)v;
    out[2] = sht_crc(out, 2);
}

void sht40_init(sht40_t *m)
{
    m->temp_c = 21.5f;
    m->humidity = 45.0f;
    m->cmd = 0;
}

void sht40_write(void *ctx, const uint8_t *data, size_t len)
{
    sht40_t *m = ctx;
    if (len) m->cmd = data[0];
}

void sht40_read(void *ctx, uint8_t *buf, size_t len)
{
    sht40_t *m = ctx;
    uint8_t out[6];
    memset(out, 0xFF, sizeof out);
    if (m->cmd == 0xFD || m->cmd == 0xF6 || m->cmd == 0xE0) {
        float t = (m->temp_c + 45.0f) * 65535.0f / 175.0f, h = (m->humidity + 6.0f) * 65535.0f / 125.0f;
        sht_word(out, (unsigned)(t < 0 ? 0 : t > 65535 ? 65535 : t + 0.5f));
        sht_word(out + 3, (unsigned)(h < 0 ? 0 : h > 65535 ? 65535 : h + 0.5f));
    } else if (m->cmd == 0x89) {
        sht_word(out, 0x1234);
        sht_word(out + 3, 0x5678);
    }
    m->cmd = 0;
    for (size_t i = 0; i < len; i++) buf[i] = i < sizeof out ? out[i] : 0xFF;
}

/* ---------------------------------------------------------------- BMI270 */

#define BMI_CONFIG_SIZE 8192

static void bmi_reset(bmi270_t *m)
{
    memset(m->reg, 0, sizeof m->reg);
    m->reg[0x00] = 0x24;
    m->reg[0x7C] = 0x03;                             /* advanced power save, as after power-up */
    m->config_bytes = 0;
    m->ready = false;
}

void bmi270_init(bmi270_t *m)
{
    memset(m, 0, sizeof *m);
    pthread_mutex_init(&m->lock, NULL);
    m->accel[2] = 1.0f;                              /* flat on a table, the screen up */
    bmi_reset(m);
}

void bmi270_set(bmi270_t *m, const float accel[3], const float gyro[3])
{
    pthread_mutex_lock(&m->lock);
    if (accel) memcpy(m->accel, accel, sizeof m->accel);
    if (gyro) memcpy(m->gyro, gyro, sizeof m->gyro);
    pthread_mutex_unlock(&m->lock);
}

bool bmi270_active(bmi270_t *m)
{
    pthread_mutex_lock(&m->lock);
    bool on = m->ready && (m->reg[0x7D] & 0x06);
    pthread_mutex_unlock(&m->lock);
    return on;
}

void bmi270_write(void *ctx, const uint8_t *d, size_t n)
{
    bmi270_t *m = ctx;
    if (!n) return;
    pthread_mutex_lock(&m->lock);
    m->ptr = d[0];
    for (size_t i = 1; i < n; i++) {
        uint8_t a = m->ptr;
        if (a == 0x5E) { m->config_bytes++; continue; }      /* the configuration file: all of a burst goes here */
        m->ptr++;
        if (a >= sizeof m->reg) continue;
        if (a == 0x7E) { if (d[i] == 0xB6) bmi_reset(m); continue; }
        m->reg[a] = d[i];
        if (a == 0x59 && (d[i] & 1)) {                       /* the file is in: check it */
            m->ready = m->config_bytes == BMI_CONFIG_SIZE;
            m->reg[0x21] = m->ready ? 0x01 : 0x02;
        }
    }
    pthread_mutex_unlock(&m->lock);
}

static void bmi_put(bmi270_t *m, uint8_t at, const float v[3], float full_scale, bool on)
{
    for (int i = 0; i < 3; i++) {
        float raw = on ? v[i] / full_scale * 32768.0f : 0;
        int16_t r = (int16_t)(raw > 32767 ? 32767 : raw < -32768 ? -32768 : lroundf(raw));
        m->reg[at + 2 * i] = (uint8_t)(r & 0xFF);
        m->reg[at + 2 * i + 1] = (uint8_t)((uint16_t)r >> 8);
    }
}

void bmi270_read(void *ctx, uint8_t *buf, size_t n)
{
    bmi270_t *m = ctx;
    pthread_mutex_lock(&m->lock);
    bmi_put(m, 0x0C, m->accel, 8.0f, m->ready && (m->reg[0x7D] & 0x04));
    bmi_put(m, 0x12, m->gyro, 2000.0f, m->ready && (m->reg[0x7D] & 0x02));
    for (size_t i = 0; i < n; i++) {
        uint8_t a = m->ptr++;
        buf[i] = a < sizeof m->reg ? m->reg[a] : 0;
    }
    pthread_mutex_unlock(&m->lock);
}

/* --------------------------------------------------------------- LTR-553 */

static void ltr_read(i2c_regs_t *r, uint8_t addr)
{
    ltr553_t *m = r->user;
    if (addr == 0x88) {                              /* the light channels are read from here on */
        bool on = r->reg[0x80] & 0x01;
        /* lux = 1.7743 * ch0 while ch1 is under 45 % of the two (the data sheet's formula) */
        long ch0 = on ? lroundf(m->lux / 1.7743f) : 0;
        if (ch0 > 65535) ch0 = 65535;
        r->reg[0x88] = r->reg[0x89] = 0;
        r->reg[0x8A] = (uint8_t)(ch0 & 0xFF);
        r->reg[0x8B] = (uint8_t)(ch0 >> 8);
    } else if (addr == 0x8D) {
        int ps = (r->reg[0x81] & 0x03) == 0x03 ? m->proximity : 0;
        r->reg[0x8D] = (uint8_t)(ps & 0xFF);
        r->reg[0x8E] = (uint8_t)((ps >> 8) & 0x07);
    } else if (addr == 0x8C) {
        r->reg[0x8C] = (uint8_t)(((r->reg[0x80] & 0x01) ? 0x04 : 0) | ((r->reg[0x81] & 0x03) == 0x03 ? 0x01 : 0));
    }
}

void ltr553_init(ltr553_t *m)
{
    i2c_regs_init(&m->r);
    m->r.user = m;
    m->r.on_read = ltr_read;
    m->r.reg[0x86] = 0x92;
    m->r.reg[0x87] = 0x05;
    m->lux = 300;                                    /* a lit room */
    m->proximity = 0;
}

void ltr553_set(ltr553_t *m, float lux, int proximity)
{
    pthread_mutex_lock(&m->r.lock);
    if (lux >= 0) m->lux = lux;
    if (proximity >= 0) m->proximity = proximity > 2047 ? 2047 : proximity;
    pthread_mutex_unlock(&m->r.lock);
}

bool ltr553_active(ltr553_t *m)
{
    pthread_mutex_lock(&m->r.lock);
    bool on = (m->r.reg[0x80] & 0x01) || (m->r.reg[0x81] & 0x03) == 0x03;
    pthread_mutex_unlock(&m->r.lock);
    return on;
}

