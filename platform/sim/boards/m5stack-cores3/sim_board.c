/*
 * Simulation of the M5Stack CoreS3 family (--model cores3se|cores3|cores3-lite):
 * device models wired like the real board, and a window drawn like the device
 * with a control panel below it for the physical inputs.
 */
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "ble_device.h"
#include "chips.h"
#include "ili9342c.h"
#include "sim_audio.h"
#include "sim_board.h"
#include "sim_hw.h"

static ili9342c_t s_lcd;
static axp2101_t s_pmic;
static aw9523_t s_ioexp;
static ft6336u_t s_touch;
static bm8563_t s_rtc;
static aw88298_t s_amp;            /* the speaker's amplifier, fed over I2S */
/* Grove Port A (I2C port 1): an ENV IV unit (SHT40 at 0x44), plugged in
   with --set porta=env. */
#define PORTA_I2C_PORT 1
#define SHT40_ADDR     0x44
static sht40_t s_sht40;
static bool s_porta_env;       /* plugged in */
static bool s_rtc_gone;        /* --set rtc=none: not on the bus */
static float s_accel[3] = { 0, 0, 1 }, s_gyro[3], s_lux = 300;   /* the sensors' inputs (--set accel, gyro, light, proximity) */
static int s_prox;
static bool s_portc_loop;      /* Port C: a jumper from TX to RX (the script's uart, UART1) */
static bool s_port_power;      /* 5V out to the ports (IO expander P0_1) */
static bool s_porta_attached;

/* The unit answers when it's plugged in and powered. */
static void update_porta(void)
{
    bool want = s_porta_env && s_port_power;
    if (want && !s_porta_attached) {
        s_sht40.cmd = 0;                              /* a fresh power-up */
        sim_i2c_attach_port(PORTA_I2C_PORT, SHT40_ADDR, sht40_write, sht40_read, &s_sht40);
    }
    if (!want && s_porta_attached) sim_i2c_detach_port(PORTA_I2C_PORT, SHT40_ADDR);
    s_porta_attached = want;
}
static bmi270_t s_imu;             /* CoreS3 and Lite: the motion sensor (the firmware also tells them from the SE by it) */
static ltr553_t s_light;           /* ... and the light and proximity sensor */

typedef enum { M_SE, M_CORES3, M_LITE } model_t;
static model_t s_model = M_SE;
static const char *const MODEL_IDS[] = { "cores3se", "cores3", "cores3-lite" };
static const char *const MODEL_NAMES[] = { "CoreS3 SE", "CoreS3", "CoreS3 Lite" };
static char s_rtc_file[600];       /* "<nvs>.rtc": the chip's time, kept like the USB power stays on */

/* Physical inputs. */
static int s_portb = 2048;      /* Grove Port B knob (e.g. an Angle unit), raw ADC */
static int s_battery = -1;      /* %, -1 = none (the SE has no battery unless a module is added) */
static int s_last_battery = 80; /* level when a battery is attached again */
static int s_usb = 1;           /* 5 V on USB-C */
static int s_wifi_ap = 1;       /* home Wi-Fi access point in range */
static int s_wifi_rssi = -52;   /* ... and its signal, dBm */
static int s_boot_touch;        /* finger on the screen during boot (safe mode) */
static int s_touch_down, s_tx, s_ty;
static int s_key_btn = -1;      /* button held via keyboard: 0..2 */
static int s_pwr_down;          /* power key held via mouse or keyboard (drawn pressed) */
static uint32_t s_pwr_at;       /* ... since (SDL ticks) */
static int s_pwr_long;          /* ... and held long enough for a long press, already sent */
#define PWR_LONG_MS 1500        /* the AXP2101's default long-press time */

/* Without a battery the board only runs while 5 V is there. */
static int usb_power(void) { return s_usb || s_battery < 0; }

static void toggle_battery(void)
{
    if (s_battery >= 0) { s_last_battery = s_battery; s_battery = -1; }
    else s_battery = s_last_battery;
}

static void apply_inputs(void)
{
    sim_adc_set_raw(BOARD_ADC_UNIT, BOARD_ADC_PORTB_IN, s_portb);
    sim_wifi_set_ap(s_wifi_ap);
    sim_wifi_set_rssi(s_wifi_rssi);
    axp2101_set_battery(&s_pmic, s_battery, usb_power());
    if (s_key_btn >= 0) {
        static const int cx[3] = { 107, 161, 215 };    /* the SE's icons */
        ft6336u_set_touch(&s_touch, true, cx[s_key_btn], 270);
    } else if (s_boot_touch) {
        ft6336u_set_touch(&s_touch, true, BOARD_LCD_W / 2, BOARD_LCD_H / 2);
    } else {
        ft6336u_set_touch(&s_touch, s_touch_down, s_tx, s_ty);
    }
}

/* The AW9523B drives the reset lines of the LCD and the touch controller. */
static void ioexp_pin(aw9523_t *m, int port, int bit, int level)
{
    (void)m;
    if (port == 1 && bit == BOARD_IOEXP_LCD_RST && level == 0) ili9342c_reset(&s_lcd);
    if (port == 0 && bit == BOARD_IOEXP_BUS_EN) {
        s_port_power = level;
        update_porta();
    }
    if (port == 0 && bit == BOARD_IOEXP_AMP_RST) aw88298_set_reset(&s_amp, level == 0);
    if (port == 0 && bit == BOARD_IOEXP_TOUCH_RST) {
        s_touch.in_reset = (level == 0);
        apply_inputs();
    }
}

bool sim_board_set_model(const char *name)
{
    for (int i = 0; i < 3; i++) {
        if (strcmp(name, MODEL_IDS[i])) continue;
        s_model = (model_t)i;
        s_battery = s_model == M_SE ? -1 : 80;        /* CoreS3 500 mAh, Lite 200 mAh */
        return true;
    }
    return false;
}

const char *sim_board_models(void) { return "cores3se (default), cores3, cores3-lite"; }

/* Powered down (the PMIC's power-off bit, or deep sleep), the board comes
   back on a press of the power key, USB power being plugged in, or the RTC
   chip's interrupt (its timer or alarm): sim_board_wake_due(). */
static atomic_bool s_wake;

static void pmic_power_off(axp2101_t *m)
{
    (void)m;
    fprintf(stderr, "[sim] the PMIC powers the board down\n");
    sim_power_off();
}

bool sim_board_sensor_on(const char *name)
{
    if (!strcmp(name, "imu")) return bmi270_active(&s_imu);
    if (!strcmp(name, "light")) return ltr553_active(&s_light);
    return false;
}

bool sim_board_wake_due(void)
{
    return sim_powered_off() && (atomic_load(&s_wake) || (!s_rtc_gone && bm8563_irq(&s_rtc)));
}

static void set_usb(bool on)
{
    if (on && !usb_power() && sim_powered_off()) atomic_store(&s_wake, true);   /* plugging it in turns the board on */
    s_usb = on;
}

static void power_key(bool long_press)
{
    if (sim_powered_off()) { atomic_store(&s_wake, true); return; }
    axp2101_power_key(&s_pmic, long_press);
}

/* The power key by mouse or keyboard, as the PMIC sees it: a long press
   once held PWR_LONG_MS, else a short press when let go. */
static void power_key_down(bool down)
{
    if (down && !s_pwr_down) { s_pwr_at = SDL_GetTicks(); s_pwr_long = 0; }
    if (!down && s_pwr_down && !s_pwr_long) power_key(false);
    s_pwr_down = down;
}

static void power_key_held(void)
{
    if (s_pwr_down && !s_pwr_long && SDL_GetTicks() - s_pwr_at >= PWR_LONG_MS) {
        s_pwr_long = 1;
        power_key(true);
    }
}

void sim_board_set_state_path(const char *base)
{
    snprintf(s_rtc_file, sizeof s_rtc_file, "%s.rtc", base);
}

/* The RTC chip got a time: keep it for the next run (a reset, not a power cut). */
static void rtc_saved(bm8563_t *m)
{
    FILE *f = *s_rtc_file ? fopen(s_rtc_file, "w") : NULL;
    if (!f) return;
    /* chip time - host time; then what it's armed with to turn the board on */
    fprintf(f, "%lld %d %lld %lld %d\n", m->offset, m->valid, m->timer_at, m->alarm_at, m->ctl2);
    fclose(f);
}

static void rtc_restore(void)
{
    FILE *f = *s_rtc_file ? fopen(s_rtc_file, "r") : NULL;
    long long offset, timer_at = 0, alarm_at = 0;
    int valid, ctl2 = 0;
    if (!f) return;
    if (fscanf(f, "%lld %d %lld %lld %d", &offset, &valid, &timer_at, &alarm_at, &ctl2) >= 2) {
        /* Powered all along (the USB cable stayed in): it kept counting,
           its timer and alarm too. */
        s_rtc.offset = offset;
        s_rtc.valid = valid != 0;
        s_rtc.timer_at = timer_at;
        s_rtc.alarm_at = alarm_at;
        s_rtc.ctl2 = (uint8_t)ctl2;
        s_rtc.r.reg[0x01] = s_rtc.ctl2;
    }
    fclose(f);
}

/* The speaker plays what reaches it: the I2S frames while the amplifier is
   on and set up for them, else nothing. */
static void speaker(void *ctx, const int16_t *frames, size_t n, int channels, uint32_t rate)
{
    (void)ctx;
    sim_audio_play(aw88298_playing(&s_amp, rate) ? frames : NULL, n, channels, rate);
}

void sim_board_init(void)
{
    ili9342c_init(&s_lcd, BOARD_LCD_W, BOARD_LCD_H, BOARD_LCD_DC);
    axp2101_init(&s_pmic);
    bmi270_init(&s_imu);                               /* (before the --set inputs; on the bus only on CoreS3 and Lite) */
    ltr553_init(&s_light);
    s_pmic.on_power_off = pmic_power_off;
    aw9523_init(&s_ioexp);
    s_ioexp.on_pin = ioexp_pin;
    ft6336u_init(&s_touch);
    bm8563_init(&s_rtc);
    sht40_init(&s_sht40);
    s_rtc.on_set = rtc_saved;
    s_rtc.on_wake_set = rtc_saved;
    rtc_restore();

    sim_spi_attach(BOARD_LCD_CS, ili9342c_spi_write, &s_lcd);
    sim_i2c_attach(BOARD_PMIC_ADDR, i2c_regs_write, i2c_regs_read, &s_pmic.r);
    sim_i2c_attach(BOARD_IOEXP_ADDR, i2c_regs_write, i2c_regs_read, &s_ioexp.r);
    sim_i2c_attach(BOARD_TOUCH_ADDR, i2c_regs_write, i2c_regs_read, &s_touch.r);
    sim_i2c_attach(BOARD_RTC_ADDR, i2c_regs_write, i2c_regs_read, &s_rtc.r);
    aw88298_init(&s_amp);
    sim_i2c_attach(BOARD_AMP_ADDR, aw88298_write, aw88298_read, &s_amp);
    sim_i2s_set_sink(BOARD_SPK_I2S_PORT, speaker, NULL);
    if (s_model != M_SE) {
        sim_i2c_attach(BOARD_IMU_ADDR, bmi270_write, bmi270_read, &s_imu);
        sim_i2c_attach(BOARD_LIGHT_ADDR, i2c_regs_write, i2c_regs_read, &s_light.r);
    }
    apply_inputs();
}

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

bool sim_board_set(const char *key, const char *val)
{
    int v = atoi(val);
    if (!strcmp(key, "portb")) s_portb = clamp(v, 0, 4095);
    else if (!strcmp(key, "battery")) s_battery = !strcmp(val, "none") || !strcmp(val, "-") ? -1 : clamp(v, 0, 100);
    else if (!strcmp(key, "battery_on")) { if ((v != 0) != (s_battery >= 0)) toggle_battery(); }   /* at its last level */
    else if (!strcmp(key, "usb")) set_usb(v != 0);
    else if (!strcmp(key, "rtc") && !strcmp(val, "lost")) {   /* the RTC chip lost power */
        bm8563_lose_power(&s_rtc);
        if (*s_rtc_file) remove(s_rtc_file);
    }
    else if (!strcmp(key, "rtc") && !strcmp(val, "none")) {   /* the RTC chip doesn't answer (a broken one) */
        sim_i2c_detach_port(0, BOARD_RTC_ADDR);
        s_rtc_gone = true;
    }
    else if (!strcmp(key, "accel") || !strcmp(key, "gyro")) {   /* what the motion sensor measures: X,Y,Z in g, or degrees a second */
        float xyz[3];
        if (sscanf(val, "%f,%f,%f", &xyz[0], &xyz[1], &xyz[2]) != 3) return false;
        if (key[0] == 'a') { memcpy(s_accel, xyz, sizeof xyz); bmi270_set(&s_imu, xyz, NULL); }
        else { memcpy(s_gyro, xyz, sizeof xyz); bmi270_set(&s_imu, NULL, xyz); }
    }
    else if (!strcmp(key, "sd")) sim_sd_set(val);       /* the card slot: a folder, unformatted or none */
    else if (!strcmp(key, "light")) { s_lux = (float)atof(val); ltr553_set(&s_light, s_lux, -1); }   /* lux */
    else if (!strcmp(key, "proximity")) { s_prox = clamp(v, 0, 2047); ltr553_set(&s_light, -1, s_prox); }
    else if (!strcmp(key, "boot_touch")) s_boot_touch = v != 0;
    else if (!strcmp(key, "porta")) {                  /* what's plugged into Port A: env|none */
        bool env = !strcmp(val, "env");
        if (!env && strcmp(val, "none") && strcmp(val, "-")) return false;
        s_porta_env = env;
        update_porta();
    }
    else if (!strcmp(key, "portc")) {                  /* what's plugged into Port C: loopback|none */
        bool loop = !strcmp(val, "loopback");
        if (!loop && strcmp(val, "none") && strcmp(val, "-")) return false;
        s_portc_loop = loop;
        sim_uart_loopback(UART_NUM_1, loop);
    }
    else if (!strcmp(key, "env_temp")) s_sht40.temp_c = (float)atof(val);
    else if (!strcmp(key, "env_hum")) s_sht40.humidity = (float)atof(val);
    else if (!strcmp(key, "power")) {                  /* press the power key: short|long */
        if (strcmp(val, "short") && strcmp(val, "long")) return false;
        power_key(!strcmp(val, "long"));
    }
    else if (!strcmp(key, "wifi")) s_wifi_ap = v != 0;
    else if (!strcmp(key, "wifi_rssi")) s_wifi_rssi = clamp(v, -100, -20);
    else if (!strcmp(key, "wifi_port") && v >= 0 && v < 65536 && *val) {   /* 0: any free port */
        sim_net_map_port(3333, (uint16_t)v);
    }
    else if (!strcmp(key, "button")) {                 /* hold a touch button: A|B|C, "-" = release */
        if (!strcmp(val, "-")) s_key_btn = -1;
        else if (val[0] >= 'A' && val[0] <= 'C' && !val[1]) s_key_btn = val[0] - 'A';
        else return false;
    } else if (!strcmp(key, "touch")) {                /* finger on the screen at X,Y; "-" = lift */
        int x, y;
        if (!strcmp(val, "-")) s_touch_down = 0;
        else if (sscanf(val, "%d,%d", &x, &y) == 2 && x >= 0 && x < BOARD_TOUCH_W && y >= 0 && y < BOARD_TOUCH_H) {
            s_tx = x; s_ty = y; s_touch_down = 1;
        } else return false;
    }
    else return false;
    apply_inputs();
    return true;
}

static const sim_board_opt_t OPTIONS[] = {
    { "Battery", "battery_on", "0|1", "None|Attached", 0, 0, NULL },
    { "Battery level", "battery", NULL, NULL, 0, 100, " %" },
    { "USB power", "usb", "0|1", "Off|On", 0, 0, NULL },
    { "Wi-Fi access point", "wifi", "0|1", "Away|In range", 0, 0, NULL },
    { "Wi-Fi signal", "wifi_rssi", NULL, NULL, -95, -30, " dBm" },
    { "Port A unit", "porta", "none|env", "None|ENV IV", 0, 0, NULL },
    { "ENV temperature", "env_temp", NULL, NULL, -20, 60, " C" },
    { "ENV humidity", "env_hum", NULL, NULL, 0, 100, " %" },
    { "Port B analog (G9)", "portb", NULL, NULL, 0, 4095, "" },
};

const sim_board_opt_t *sim_board_options(int *n)
{
    *n = (int)(sizeof OPTIONS / sizeof OPTIONS[0]);
    return OPTIONS;
}

int sim_board_get(const char *key)
{
    if (!strcmp(key, "battery_on")) return s_battery >= 0;
    if (!strcmp(key, "battery")) return s_battery >= 0 ? s_battery : s_last_battery;
    if (!strcmp(key, "usb")) return s_usb;
    if (!strcmp(key, "wifi")) return s_wifi_ap;
    if (!strcmp(key, "wifi_rssi")) return s_wifi_rssi;
    if (!strcmp(key, "porta")) return s_porta_env;
    if (!strcmp(key, "env_temp")) return (int)(s_sht40.temp_c + (s_sht40.temp_c < 0 ? -0.5f : 0.5f));
    if (!strcmp(key, "env_hum")) return (int)(s_sht40.humidity + 0.5f);
    if (!strcmp(key, "portb")) return s_portb;
    return 0;
}

const char *sim_board_set_help(void)
{
    return "portb=0..4095 porta=env|none portc=loopback|none sd=DIR|unformatted|none accel=X,Y,Z gyro=X,Y,Z light=LUX proximity=0..2047 env_temp=C env_hum=% battery=none|0..100 battery_on=0|1 usb=0|1 rtc=lost|none boot_touch=0|1 wifi=0|1 wifi_rssi=dBm wifi_port=N|0 button=A|B|C|- touch=X,Y|- power=short|long";
}

void sim_board_state(char *out, size_t cap)
{
    /* battery first: setting a level attaches it, battery_on=0 takes it off again.
       The Wi-Fi port is the one bound, so a reboot keeps it, as a PC
       reconnecting expects. */
    snprintf(out, cap,
             "battery=%d battery_on=%d usb=%d wifi=%d wifi_rssi=%d wifi_port=%d porta=%s portc=%s env_temp=%.2f env_hum=%.2f portb=%d"
             " accel=%.3f,%.3f,%.3f gyro=%.2f,%.2f,%.2f light=%.1f proximity=%d%s",
             s_battery >= 0 ? s_battery : s_last_battery, s_battery >= 0, s_usb, s_wifi_ap, s_wifi_rssi,
             sim_net_host_port(3333),
             s_porta_env ? "env" : "none", s_portc_loop ? "loopback" : "none", (double)s_sht40.temp_c,
             (double)s_sht40.humidity, s_portb, (double)s_accel[0], (double)s_accel[1], (double)s_accel[2],
             (double)s_gyro[0], (double)s_gyro[1], (double)s_gyro[2], (double)s_lux, s_prox,
             s_rtc_gone ? " rtc=none" : "");
    /* The card stays in over a reset (a folder with a space in its name can't be kept this way). */
    char sd[512];
    sim_sd_get(sd, sizeof sd);
    size_t n = strlen(out);
    if (!strchr(sd, ' ') && n < cap) snprintf(out + n, cap - n, " sd=%s", sd);
    /* A finger held on the screen stays there while the board starts (safe
       mode after a power-off: touch=X,Y, then power=short). */
    n = strlen(out);
    if (s_touch_down && n < cap) snprintf(out + n, cap - n, " touch=%d,%d", s_tx, s_ty);
}

bool sim_board_boot_done(bool late)
{
    /* The finger of boot_touch stays until the firmware has looked for one
       once more after the boot's 300 ms: on a busy computer its start takes
       longer than that, and it looks more than once before it decides on
       safe mode (the first look comes before the 300 ms even then). A
       firmware that never looks again lets it go 5 s into the boot (late). */
    static unsigned looked = UINT_MAX;                   /* its looks when the 300 ms were over */
    if (s_boot_touch && !late) {
        unsigned now = ft6336u_reads(&s_touch);
        if (looked == UINT_MAX) looked = now;
        if (now == looked) return false;
    }
    s_boot_touch = 0;
    apply_inputs();
    return true;
}

bool sim_board_screen_rgb(uint8_t *rgb, int *w, int *h)
{
    ili9342c_view_rgb(&s_lcd, rgb);
    *w = BOARD_LCD_W;
    *h = BOARD_LCD_H;
    return true;
}

/* ------------------------------------------------------------------ window */

/* Everything is in multiples of the scale, so the window keeps its shape. */
#define MARGIN   10
#define PANEL_H  30

typedef struct {
    int s;                   /* scale */
    int m;                   /* margin */
    int bx, by, bw, bh;      /* body */
    int lx, ly;              /* LCD origin */
    int gx, gy, gw, gh;      /* glass (LCD + touch strip) */
    ui_rect_t settings, pwr;
    int panel_y;
} layout_t;

static layout_t layout(int s)
{
    layout_t L;
    L.s = s;
    /* Measured from product photos, in LCD pixels: a 414 x 414 body with the
       LCD 47 from the sides and 86 from the top, on all three models. */
    L.m = MARGIN * s;
    L.bw = L.bh = 414 * s;
    L.bx = L.m;
    L.by = L.m;
    L.lx = L.bx + 47 * s;
    L.ly = L.by + 86 * s;
    L.pwr = (ui_rect_t){ L.bx - 3 * s, L.by + 70 * s, 5 * s, 26 * s };   /* left side */
    int rim = (s_model == M_SE ? 8 : 10) * s;          /* plastic or metal around the glass */
    L.gx = L.bx + rim;
    L.gy = L.by + rim;
    L.gw = L.bw - 2 * rim;
    L.gh = L.bh - 2 * rim;                             /* glass covers LCD + touch strip */
    L.panel_y = L.by + L.bh + 6 * s;
    /* the status line | SETTINGS, then the keys */
    L.settings = (ui_rect_t){ L.m + L.bw - 56 * s, L.panel_y + 4 * s, 56 * s, 11 * s };
    return L;
}

void sim_board_ui_size(int scale, int *w, int *h)
{
    layout_t L = layout(scale);
    *w = L.bw + 2 * L.m;
    *h = L.panel_y + PANEL_H * scale;
}

static int zone_of(int x) { return x < BOARD_BTN_B_X0 ? 0 : x < BOARD_BTN_C_X0 ? 1 : 2; }

/* A thin chevron pointing left (dir -1) or right (dir 1), 7 x 14 LCD pixels. */
static void chevron(sim_ui_t *ui, int cx, int cy, int dir, int s, uint32_t c)
{
    int w = 7 * s / 2, h = 7 * s, t = s + s / 2 > 2 ? s + s / 2 : 2;
    ui_color(ui, c, 255);
    for (int k = 0; k < t; k++) {
        int tip = cx + dir * w - dir * k, back = cx - dir * w - dir * k;
        SDL_RenderDrawLine(ui->r, back, cy - h, tip, cy);
        SDL_RenderDrawLine(ui->r, tip, cy, back, cy + h);
    }
}

static int touch_at(const layout_t *L, int mx, int my, int *tx, int *ty)
{
    int x = (mx - L->lx) / L->s, y = (my - L->ly) / L->s;
    if (mx < L->lx || my < L->ly || x >= BOARD_TOUCH_W || y >= BOARD_TOUCH_H) return 0;
    *tx = x;
    *ty = y;
    return 1;
}

/* A touch button held from the keyboard; releasing another key doesn't let go of it. */
static void key_button(int b, int down)
{
    if (down) s_key_btn = b;
    else if (s_key_btn == b) s_key_btn = -1;
}

void sim_board_ui_event(sim_ui_t *ui, const SDL_Event *e)
{
    layout_t L = layout(ui->scale);
    int step;
    switch (e->type) {
    case SDL_MOUSEBUTTONDOWN: {
        int x = e->button.x, y = e->button.y;
        ui_rect_t ph = { L.pwr.x - 6, L.pwr.y - 4, L.pwr.w + 12, L.pwr.h + 8 };
        if (ui_hit(ph, x, y)) power_key_down(true);
        else if (touch_at(&L, x, y, &s_tx, &s_ty)) s_touch_down = 1;
        else if (ui_hit(L.settings, x, y)) ui->settings_open = true;
        break;
    }
    case SDL_MOUSEMOTION:
        if (s_touch_down) {
            int tx, ty;
            if (touch_at(&L, e->motion.x, e->motion.y, &tx, &ty)) { s_tx = tx; s_ty = ty; }
            else s_touch_down = 0;   /* finger slid off the glass */
        }
        break;
    case SDL_MOUSEBUTTONUP:
        s_touch_down = 0;
        power_key_down(false);
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        int down = e->type == SDL_KEYDOWN;
        step = (e->key.keysym.mod & KMOD_SHIFT) ? 256 : 32;
        switch (e->key.keysym.sym) {
        /* touch buttons A/B/C (< o >; "up open down" in the menus): also Left or Up, Enter, Right or Down */
        case SDLK_a: case SDLK_LEFT: case SDLK_UP: key_button(0, down); break;
        case SDLK_b: case SDLK_RETURN: case SDLK_KP_ENTER: key_button(1, down); break;
        case SDLK_c: case SDLK_RIGHT: case SDLK_DOWN: key_button(2, down); break;
        case SDLK_PERIOD: if (down) s_portb = clamp(s_portb + step, 0, 4095); break;
        case SDLK_COMMA:  if (down) s_portb = clamp(s_portb - step, 0, 4095); break;
        case SDLK_PLUS: case SDLK_EQUALS: case SDLK_KP_PLUS:   /* + is its own key on Nordic layouts, = on US */
            if (down) s_battery = s_battery < 0 ? s_last_battery : clamp(s_battery + 5, 0, 100);
            break;
        case SDLK_MINUS: case SDLK_KP_MINUS:
            if (down) s_battery = s_battery < 0 ? s_last_battery : clamp(s_battery - 5, 0, 100);
            break;
        case SDLK_x:     if (down) toggle_battery(); break;
        case SDLK_p:     if (!e->key.repeat) power_key_down(down); break;
        case SDLK_u:     if (down) set_usb(!s_usb); break;
        case SDLK_w:     if (down) s_wifi_ap = !s_wifi_ap; break;
        case SDLK_d:     if (down && !e->key.repeat) ble_device_toggle_all(); break;   /* simulated Bluetooth devices */
        default: break;
        }
        break;
    }
    default:
        break;
    }
    apply_inputs();
}

void sim_board_ui_draw(sim_ui_t *ui)
{
    power_key_held();
    layout_t L = layout(ui->scale);
    int s = L.s;
    ui_fill(ui, 0, 0, L.bw + 2 * L.m, L.panel_y + PANEL_H * s, 0x16191d);

    /* Body with a soft edge and the side power key: white plastic on the SE,
       dark gray on CoreS3/Lite. */
    int se = s_model == M_SE;
    uint32_t key = s_pwr_down ? 0x6a8fb0 : se ? 0xc4c7cb : 0x4a4c50;
    ui_round_rect(ui, L.pwr.x + (s_pwr_down ? s : 0), L.pwr.y, L.pwr.w, L.pwr.h, 2 * s, key);
    ui_round_rect(ui, L.bx, L.by, L.bw, L.bh, 26 * s, se ? 0xc9ccd0 : 0x4a4c50);
    ui_round_rect(ui, L.bx + s, L.by + s, L.bw - 2 * s, L.bh - 2 * s, 25 * s, se ? 0xeef0f2 : 0x6a6c70);

    /* Black glass over almost the whole front: LCD, touch strip and its marks. */
    ui_round_rect(ui, L.gx, L.gy, L.gw, L.gh, 18 * s, 0x0b0c0e);
    static uint8_t rgb[BOARD_LCD_W * BOARD_LCD_H * 3];
    ili9342c_view_rgb(&s_lcd, rgb);
    ui_lcd(ui, rgb, BOARD_LCD_W, BOARD_LCD_H, L.lx, L.ly, sim_powered_off() ? 0.0f : axp2101_backlight(&s_pmic));

    if (!se) {
        /* CoreS3/Lite: no printed icons. Mic holes, camera, light/proximity
           sensor, placed as on the device (in touch coordinates). */
        int y = L.ly + 278 * s;
        ui_circle(ui, L.lx + 52 * s, y, 3 * s, 0x26282c);             /* mic holes */
        ui_circle(ui, L.lx + 52 * s, y, 2 * s, 0x08090a);
        ui_circle(ui, L.lx + 269 * s, y, 3 * s, 0x26282c);
        ui_circle(ui, L.lx + 269 * s, y, 2 * s, 0x08090a);
        ui_circle(ui, L.lx + 158 * s, y + 3 * s, 14 * s, 0x9a5a35);   /* camera ring */
        ui_circle(ui, L.lx + 158 * s, y + 3 * s, 11 * s, 0x15161a);
        ui_circle(ui, L.lx + 158 * s, y + 3 * s, 5 * s, 0x2a3440);    /* lens */
        ui_fill(ui, L.lx + 210 * s, y + 3 * s, 4 * s, 4 * s, 0x2a2c30);  /* light/proximity sensor */
        ui_circle(ui, L.lx + 230 * s, y + 5 * s, 2 * s + s / 2, 0x2a2c30);
        if (s_touch_down && s_ty >= BOARD_LCD_H) {                    /* finger on the strip */
            SDL_SetRenderDrawBlendMode(ui->r, SDL_BLENDMODE_BLEND);
            ui_color(ui, 0xffffff, 50);
            for (int r = 9 * s; r > 0; r -= 2) {
                SDL_Rect rc = { L.lx + s_tx * s - r, L.ly + s_ty * s - r, 2 * r, 2 * r };
                SDL_RenderDrawRect(ui->r, &rc);
            }
            SDL_SetRenderDrawBlendMode(ui->r, SDL_BLENDMODE_NONE);
        }
    }
    if (se) {
        /* SE: icons printed on the glass, positions from a product photo:
           "<  <   o   >  >" in orange-red (brighter while touched), and a
           small hole at each end. */
        int cy = L.ly + 270 * s;
        int zone = s_key_btn >= 0 ? s_key_btn : s_touch_down && s_ty >= BOARD_LCD_H ? zone_of(s_tx) : -1;
        uint32_t lit = 0xff8a5c, dim = 0xd04a2c;
        chevron(ui, L.lx + 93 * s, cy, -1, s, zone == 0 ? lit : dim);
        chevron(ui, L.lx + 122 * s, cy, -1, s, zone == 0 ? lit : dim);
        chevron(ui, L.lx + 201 * s, cy, 1, s, zone == 2 ? lit : dim);
        chevron(ui, L.lx + 229 * s, cy, 1, s, zone == 2 ? lit : dim);
        ui_circle(ui, L.lx + 161 * s, cy, 11 * s, zone == 1 ? lit : dim);
        ui_circle(ui, L.lx + 161 * s, cy, 11 * s - (s + s / 2 > 2 ? s + s / 2 : 2), 0x0b0c0e);
        for (int x = 54; x <= 270; x += 216) {
            ui_circle(ui, L.lx + x * s, L.ly + 276 * s, 4 * s + s / 2, 0x2c2e31);
            ui_circle(ui, L.lx + x * s, L.ly + 276 * s, 3 * s, 0x060708);
        }
    }
    /* Finger indicator while touching the LCD. */
    if (s_touch_down && s_ty < BOARD_LCD_H) {
        SDL_SetRenderDrawBlendMode(ui->r, SDL_BLENDMODE_BLEND);
        ui_color(ui, 0xffffff, 60);
        for (int r = 9 * s; r > 0; r -= 2) {
            SDL_Rect rc = { L.lx + s_tx * s - r, L.ly + s_ty * s - r, 2 * r, 2 * r };
            SDL_RenderDrawRect(ui->r, &rc);
        }
        SDL_SetRenderDrawBlendMode(ui->r, SDL_BLENDMODE_NONE);
    }

    /* Charge LED (red while charging), next to the light sensor on CoreS3/Lite. */
    if (!se && s_battery >= 0 && usb_power() && s_battery < 100)
        ui_circle(ui, L.lx + 230 * s, L.ly + 283 * s, 2 * s, 0xff3030);

    /* Control panel: what's going on, the settings, the keys. The inputs
       themselves are in the settings (OPTIONS). */
    int ty = L.panel_y, small = (s + 1) / 2;
    int wifi_up = sim_wifi_connected();
    ui_button(ui, L.settings, "SETTINGS", s, ui->settings_open ? 0x4f6a85 : 0x3a4048);
    char batt[24], line[320];
    if (s_battery >= 0) snprintf(batt, sizeof batt, "battery %d%%", s_battery);
    else snprintf(batt, sizeof batt, "no battery");
    snprintf(line, sizeof line, "%s   %s%s   USB serial %s   Wi-Fi tcp://127.0.0.1:%d%s", MODEL_NAMES[s_model],
             batt, usb_power() ? ", USB 5V" : "", ui->tty_path, sim_net_host_port(3333),
             !s_wifi_ap ? " (no access point)" : wifi_up ? "" : " (not joined)");
    ui_text(ui, L.m, ty + 6 * s, line, ui_text_fit(line, s, L.settings.x - L.m - 4 * s), 0xc8d2dc);
    const char *help = "A/B/C, arrows, Enter: touch buttons  P: power  - + X: battery  U: USB  "
                       "W: Wi-Fi  S: settings  Esc: quit";
    small = ui_text_fit(help, small, L.bw);
    if (ui_text_w(help, small) <= L.bw) ui_text(ui, L.m, ty + 21 * s, help, small, 0x66737f);   /* not at the smallest */
}
