#include "lua_api.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "builtin_lua.h"
#include "ota.h"
#include "board.h"
#include "clock.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gfx.h"
#include "buttons.h"
#include "hw.h"
#include "jpeg.h"
#include "png.h"
#include "lauxlib.h"
#include "lua_i2c.h"
#include "lua_pins.h"
#include "lua_uart.h"
#include "screens.h"
#include "sha256.h"
#include "script.h"
#include "settings.h"
#include "power.h"
#include "sleep.h"
#include "vfs.h"

/* Lua sees colors as 0xRRGGBB integers; the framebuffer stores RGB565. */
#define RGB_WHITE 0xFFFFFF
#define RGB_BLACK 0x000000

/* ================================================================ disp */

static gfx_color_t opt_color(lua_State *L, int idx, uint32_t def)
{
    lua_Integer c = luaL_optinteger(L, idx, (lua_Integer)def);
    luaL_argcheck(L, c >= 0 && c <= 0xFFFFFF, idx, "color must be 0xRRGGBB (0..0xFFFFFF)");
    return gfx_from_rgb888((uint32_t)c);
}

/* An int argument; out-of-range values saturate instead of wrapping, so the
   range checks after it see what the script passed. */
static int ci(lua_State *L, int idx)
{
    lua_Integer v = luaL_checkinteger(L, idx);
    return (int)(v < INT_MIN ? INT_MIN : v > INT_MAX ? INT_MAX : v);
}

/* A coordinate or size: clamped so huge values can't overflow the drawing
   code (and a far-off-screen shape still costs next to nothing). */
static int coord(lua_State *L, int idx)
{
    lua_Integer v = luaL_checkinteger(L, idx);
    return (int)(v < -GFX_COORD_MAX ? -GFX_COORD_MAX : v > GFX_COORD_MAX ? GFX_COORD_MAX : v);
}

/* The firmware shows a notice when a script draws nothing; the first drawing
   call takes the screen back (black, as if the notice was never there). */
static bool s_drew;
bool lua_api_drew(void) { return s_drew; }
static void drawing(void)
{
    if (s_drew) return;
    s_drew = true;
    if (screen_notice_active()) { gfx_fill(0); screen_notice_clear(); }
}

int lua_api_coord(lua_State *L, int idx) { return coord(L, idx); }
void lua_api_drawing(void) { drawing(); }

static int disp_clear(lua_State *L) { drawing(); gfx_fill(opt_color(L, 1, RGB_BLACK)); return 0; }
static int disp_show(lua_State *L)  { (void)L; gfx_flush(); return 0; }

static int disp_rgb(lua_State *L)
{
    lua_Integer r = luaL_checkinteger(L, 1), g = luaL_checkinteger(L, 2), b = luaL_checkinteger(L, 3);
    luaL_argcheck(L, r >= 0 && r <= 255, 1, "0..255");
    luaL_argcheck(L, g >= 0 && g <= 255, 2, "0..255");
    luaL_argcheck(L, b >= 0 && b <= 255, 3, "0..255");
    lua_pushinteger(L, (r << 16) | (g << 8) | b);
    return 1;
}

static int disp_pixel(lua_State *L)
{
    drawing();
    gfx_pixel(coord(L, 1), coord(L, 2), opt_color(L, 3, RGB_WHITE));
    return 0;
}

static int disp_get(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)gfx_to_rgb888(gfx_get_pixel(coord(L, 1), coord(L, 2))));
    return 1;
}

static int disp_line(lua_State *L)
{
    drawing();
    gfx_line(coord(L, 1), coord(L, 2), coord(L, 3), coord(L, 4), opt_color(L, 5, RGB_WHITE));
    return 0;
}

/* A flag: none or nil is def, anything else must be a boolean (so a 0
   meant as false is an error instead of counting as true). */
bool lua_api_optbool(lua_State *L, int idx, bool def)
{
    if (lua_isnoneornil(L, idx)) return def;
    luaL_checktype(L, idx, LUA_TBOOLEAN);
    return lua_toboolean(L, idx);
}

static int disp_rect(lua_State *L)
{
    drawing();
    gfx_rect(coord(L, 1), coord(L, 2), coord(L, 3), coord(L, 4), opt_color(L, 5, RGB_WHITE), lua_api_optbool(L, 6, false));
    return 0;
}

static int disp_circle(lua_State *L)
{
    drawing();
    gfx_circle(coord(L, 1), coord(L, 2), coord(L, 3), opt_color(L, 4, RGB_WHITE), lua_api_optbool(L, 5, false));
    return 0;
}

static int disp_text(lua_State *L)
{
    int x = coord(L, 1), y = coord(L, 2);
    luaL_checkany(L, 3);
    /* Read optional args before luaL_tolstring pushes onto the stack. */
    gfx_color_t fg = opt_color(L, 4, RGB_WHITE);
    lua_Integer scale = luaL_optinteger(L, 5, 1);
    luaL_argcheck(L, scale >= 1 && scale <= 8, 5, "scale must be 1..8");
    bool has_bg = !lua_isnoneornil(L, 6);
    gfx_color_t bg = has_bg ? opt_color(L, 6, RGB_BLACK) : 0;
    static const char *const aligns[] = { "left", "center", "right", NULL };
    int align = luaL_checkoption(L, 7, "left", aligns);
    const char *s = luaL_tolstring(L, 3, NULL);
    drawing();
    if (!align) {
        lua_pushinteger(L, gfx_text(x, y, s, fg, (int)scale, has_bg ? &bg : NULL));
        return 1;
    }
    /* Centered on x, or ending at x: each line by itself. */
    int maxw = 0;
    for (;;) {
        const char *nl = strchr(s, '\n');
        lua_pushlstring(L, s, nl ? (size_t)(nl - s) : strlen(s));
        const char *line = lua_tostring(L, -1);
        int w = gfx_text_width(line, (int)scale);
        gfx_text(align == 1 ? x - (w + 1) / 2 : x - w, y, line, fg, (int)scale, has_bg ? &bg : NULL);
        lua_pop(L, 1);
        if (w > maxw) maxw = w;
        if (!nl) break;
        s = nl + 1;
        if (y < GFX_COORD_MAX) y += 8 * (int)scale;
    }
    lua_pushinteger(L, maxw);
    return 1;
}

static int disp_text_width(lua_State *L)
{
    luaL_checkany(L, 1);
    lua_Integer scale = luaL_optinteger(L, 2, 1);
    luaL_argcheck(L, scale >= 1 && scale <= 8, 2, "scale must be 1..8");
    const char *s = luaL_tolstring(L, 1, NULL);
    lua_pushinteger(L, gfx_text_width(s, (int)scale));
    return 1;
}

static int disp_image(lua_State *L)
{
    int x = coord(L, 1), y = coord(L, 2), w = coord(L, 3), h = coord(L, 4);
    size_t len;
    const char *px = luaL_checklstring(L, 5, &len);
    luaL_argcheck(L, w > 0 && h > 0 && w <= 4096 && h <= 4096, 3, "bad image size");
    if (len != (size_t)w * h * 2)
        return luaL_error(L, "disp.image: %dx%d needs %d bytes of RGB565, got %d", w, h, w * h * 2, (int)len);
    drawing();
    gfx_blit_be565(x, y, w, h, (const uint8_t *)px);
    return 0;
}

/* disp.grab(x, y, w, h) -> the screen's rectangle as disp.image takes it
   (big-endian RGB565, rows top to bottom); off the screen is black. */
static int disp_grab(lua_State *L)
{
    int x = coord(L, 1), y = coord(L, 2), w = coord(L, 3), h = coord(L, 4);
    luaL_argcheck(L, w > 0 && h > 0 && w <= 4096 && h <= 4096, 3, "bad image size");
    luaL_Buffer b;
    uint8_t *out = (uint8_t *)luaL_buffinitsize(L, &b, (size_t)w * (size_t)h * 2);
    int sw = gfx_width(), sh = gfx_height();
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int px = x + i, py = y + j;
            gfx_color_t c = px >= 0 && px < sw && py >= 0 && py < sh ? gfx_get_pixel(px, py) : 0;
            *out++ = (uint8_t)(c >> 8);
            *out++ = (uint8_t)c;
        }
    luaL_pushresultsize(&b, (size_t)w * (size_t)h * 2);
    return 1;
}

/* disp.brightness([0..100]) -> current level. Starts at the Brightness
   setting; goes back to it when the script ends. */
static int s_level = -1;
static int disp_brightness(lua_State *L)
{
    if (!lua_isnoneornil(L, 1)) {
        int v = ci(L, 1);
        luaL_argcheck(L, v >= 0 && v <= 100, 1, "brightness must be 0..100");
        if (hw_backlight(v) != ESP_OK) return luaL_error(L, "backlight control failed");
        s_level = v;
    }
    lua_pushinteger(L, s_level >= 0 ? s_level : settings_brightness());
    return 1;
}

/* disp.keep_awake([on]) -> on. While on, the Sleep after setting doesn't
   turn the screen off (the user still can). Off again when the script ends. */
static int disp_keep_awake(lua_State *L)
{
    if (!lua_isnoneornil(L, 1)) sleep_keep_awake(lua_api_optbool(L, 1, false));
    lua_pushboolean(L, sleep_kept_awake());
    return 1;
}

/* disp.sleep([on]) -> on. true: asleep, as by Settings > Sleep now. false:
   awake, as by a touch (so the Sleep after time starts over). */
static int disp_sleep(lua_State *L)
{
    if (!lua_isnoneornil(L, 1)) {
        if (lua_api_optbool(L, 1, false)) sleep_now();
        else sleep_activity(hw_millis());
    }
    lua_pushboolean(L, sleep_active());
    return 1;
}

typedef const char *image_draw_fn(int x, int y, const uint8_t *data, size_t len, int shift, int *w, int *h);
typedef const char *image_size_fn(const uint8_t *data, size_t len, int *w, int *h);

/* w, h, or nil and why (bad data, not a bad call: an error value) */
static int image_result(lua_State *L, const char *name, const char *e, int w, int h)
{
    if (e) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s: %s", name, e);
        return 2;
    }
    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
    return 2;
}

/* disp.<name>(x, y, data, [scale 1|2|4|8]) -> w, h, or nil and why.
   `credit_ms`: watchdog time a big image may take on the chip. */
static int image_draw(lua_State *L, const char *name, image_draw_fn *draw, uint32_t credit_ms)
{
    int x = coord(L, 1), y = coord(L, 2);
    size_t len;
    const uint8_t *data = (const uint8_t *)luaL_checklstring(L, 3, &len);
    lua_Integer scale = luaL_optinteger(L, 4, 1);
    int shift;
    switch (scale) {
    case 1: shift = 0; break;
    case 2: shift = 1; break;
    case 4: shift = 2; break;
    case 8: shift = 3; break;
    default: return luaL_argerror(L, 4, "scale must be 1, 2, 4 or 8");
    }
    drawing();
    uint32_t t0 = hw_millis();
    int w = 0, h = 0;
    const char *e = draw(x, y, data, len, shift, &w, &h);
    script_watchdog_credit(L, t0, credit_ms);
    return image_result(L, name, e, w, h);
}

/* disp.<name>_size(data) -> w, h, or nil and why */
static int image_size(lua_State *L, const char *name, image_size_fn *size)
{
    size_t len;
    const uint8_t *data = (const uint8_t *)luaL_checklstring(L, 1, &len);
    int w = 0, h = 0;
    const char *e = size(data, len, &w, &h);
    return image_result(L, name, e, w, h);
}

static int disp_jpeg(lua_State *L) { return image_draw(L, "disp.jpeg", jpeg_draw, 2000); }
static int disp_jpeg_size(lua_State *L) { return image_size(L, "disp.jpeg_size", jpeg_size); }
/* a 1000x600 PNG inflates for a while on the chip */
static int disp_png(lua_State *L) { return image_draw(L, "disp.png", png_draw, 5000); }
static int disp_png_size(lua_State *L) { return image_size(L, "disp.png_size", png_size); }

static const luaL_Reg disp_funcs[] = {
    { "clear", disp_clear }, { "show", disp_show }, { "rgb", disp_rgb },
    { "pixel", disp_pixel }, { "get", disp_get }, { "line", disp_line },
    { "rect", disp_rect }, { "circle", disp_circle }, { "text", disp_text },
    { "text_width", disp_text_width }, { "image", disp_image }, { "grab", disp_grab },
    { "jpeg", disp_jpeg }, { "jpeg_size", disp_jpeg_size }, { "png", disp_png }, { "png_size", disp_png_size },
    { "brightness", disp_brightness }, { "keep_awake", disp_keep_awake }, { "sleep", disp_sleep },
    { NULL, NULL },
};

/* ================================================================ touch */

/* touch.read() and touch.button(): the touch in progress as buttons.c has
   it, which in an app's frame stays as the frame began, like its events. */
static int touch_read(lua_State *L)
{
    int x, y;
    if (buttons_touch(&x, &y)) {
        lua_pushinteger(L, x);
        lua_pushinteger(L, y);
        return 2;
    }
    lua_pushnil(L);
    return 1;
}

static int touch_button(lua_State *L)
{
    char name[2] = { buttons_held(), 0 };
    if (name[0]) lua_pushstring(L, name);
    else lua_pushnil(L);
    return 1;
}

/* touch._event() -> kind, ... | nil: the next press/long/tap/hold/drag/swipe
   (buttons.c decides which), for the prelude's touch.on() handlers.
   press, long: "A"|"B"|"C"; tap, hold: x, y; drag: x, y, x0, y0, ended;
   swipe: "up"|"down"|"left"|"right", x, y (where it started). Repeats and
   hold-ups are the menu's. */
static int touch_event(lua_State *L)
{
    btn_event_t e;
    do {
        if (!buttons_next(&e)) return 0;
    } while (e.kind == BTN_REPEAT || e.kind == BTN_HOLD_UP);
    static const char *kinds[] = { [BTN_PRESS] = "press", [BTN_LONG] = "long", [BTN_TAP] = "tap",
                                   [BTN_HOLD] = "hold", [BTN_SWIPE] = "swipe", [BTN_DRAG] = "drag" };
    lua_pushstring(L, kinds[e.kind]);
    if (e.kind == BTN_PRESS || e.kind == BTN_LONG) {
        char b[2] = { e.what, 0 };
        lua_pushstring(L, b);
        return 2;
    }
    if (e.kind == BTN_SWIPE) {
        lua_pushstring(L, e.what == 'u' ? "up" : e.what == 'd' ? "down" : e.what == 'l' ? "left" : "right");
        lua_pushinteger(L, e.x0);
        lua_pushinteger(L, e.y0);
        return 4;
    }
    lua_pushinteger(L, e.x);
    lua_pushinteger(L, e.y);
    if (e.kind == BTN_DRAG) {
        lua_pushinteger(L, e.x0);
        lua_pushinteger(L, e.y0);
        lua_pushboolean(L, e.what == 'e');
        return 6;
    }
    return 3;
}

static const luaL_Reg touch_funcs[] = {
    { "read", touch_read }, { "button", touch_button }, { "_event", touch_event }, { NULL, NULL },
};

/* ================================================================= adc */

static void pwm_stop_pin(int pin);

/* The pin argument: GPIO1..10, the ESP32-S3's ADC1 pins. Returns the channel. */
static int adc_channel_arg(lua_State *L, int idx)
{
    int pin = ci(L, idx);
    luaL_argcheck(L, pin >= 1 && pin <= 10, idx, "an analog pin is GPIO1..10");
    if (hw_gpio_is_reserved(pin)) luaL_error(L, "GPIO%d is reserved by the firmware", pin);
    pin_check(L, pin, PIN_GPIO);
    pwm_stop_pin(pin);                                /* an analog input doesn't pulse */
    pin_take(pin, PIN_GPIO);
    return pin - 1;                                   /* ADC1 channel n is GPIO n+1 */
}

static int adc_read(lua_State *L)
{
    int raw;
    if (hw_adc_read(adc_channel_arg(L, 1), &raw) != ESP_OK) return luaL_error(L, "adc read failed");
    lua_pushinteger(L, raw);
    return 1;
}

static int adc_read_mv(lua_State *L)
{
    int raw;
    if (hw_adc_read(adc_channel_arg(L, 1), &raw) != ESP_OK) return luaL_error(L, "adc read failed");
    lua_pushinteger(L, hw_adc_raw_to_mv(raw));
    return 1;
}

static const luaL_Reg adc_funcs[] = {
    { "read", adc_read }, { "read_mv", adc_read_mv }, { NULL, NULL },
};

/* ================================================================ gpio */

enum { MODE_IN = 0, MODE_OUT = 1, MODE_IN_PULLUP = 2, MODE_IN_PULLDOWN = 3 };

/* The pins gpio.mode() made outputs (or a write that ended the pulses):
   gpio.write and gpio.toggle on any other pin would do nothing. */
static uint64_t s_out;

static int gpio_pin_arg(lua_State *L, int idx)
{
    int pin = ci(L, idx);
    luaL_argcheck(L, GPIO_IS_VALID_GPIO(pin), idx, "invalid GPIO number");
    if (hw_gpio_is_reserved(pin))
        luaL_error(L, "GPIO%d is reserved by the firmware (LCD/I2C/USB/flash)", pin);
    pin_check(L, pin, PIN_GPIO);                      /* (kept until the script ends) */
    pin_take(pin, PIN_GPIO);
    return pin;
}

/* ---- gpio.pwm: a pin pulsed by the chip's PWM unit (LEDC) ----------------
   Up to PWM_PINS pins at up to PWM_TIMERS different frequencies (a timer
   each; pins at the same frequency share one). The duty has as many steps
   as the frequency leaves, 13 bits at most: 13 up to about 9.7 kHz, 10 at
   40 kHz. Not the chip's 14: at its top resolution a duty of 100 % isn't
   reachable (a hardware flaw ESP-IDF's ledc.c notes). VERIFY on hardware. */
#define PWM_PINS    LEDC_CHANNEL_MAX
#define PWM_TIMERS  LEDC_TIMER_MAX
#define PWM_CLK_HZ  80000000u                   /* the APB clock */
#define PWM_BITS    13
#define PWM_HZ_MIN  10                          /* below it the clock can't be divided far enough */
#define PWM_HZ_MAX  40000

static struct { bool on; int pin; int timer; lua_Number duty; } s_pwm[PWM_PINS];
static struct { uint32_t hz; int bits; int users; } s_pwm_timer[PWM_TIMERS];

static int pwm_find(int pin)
{
    for (int i = 0; i < PWM_PINS; i++)
        if (s_pwm[i].on && s_pwm[i].pin == pin) return i;
    return -1;
}

/* Stop the PWM on this pin, if any: the pin is no longer driven. */
static void pwm_stop_pin(int pin)
{
    int ch = pwm_find(pin);
    if (ch < 0) return;
    ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch, 0);
    hw_gpio_release(pin);
    s_out &= ~(1ULL << pin);
    s_pwm_timer[s_pwm[ch].timer].users--;
    s_pwm[ch].on = false;
}

bool lua_api_pwm_running(void)
{
    for (int i = 0; i < PWM_PINS; i++)
        if (s_pwm[i].on) return true;
    return false;
}

/* gpio.pwm(pin) -> hz, duty | nil; gpio.pwm(pin, hz, duty); gpio.pwm(pin, 0) stops. */
static int l_gpio_pwm(lua_State *L)
{
    int pin = gpio_pin_arg(L, 1);
    int ch = pwm_find(pin);
    if (lua_isnone(L, 2)) {
        if (ch < 0) { lua_pushnil(L); return 1; }
        lua_pushinteger(L, (lua_Integer)s_pwm_timer[s_pwm[ch].timer].hz);
        lua_pushnumber(L, s_pwm[ch].duty);
        return 2;
    }
    lua_Integer hz = luaL_checkinteger(L, 2);
    if (hz == 0) { pwm_stop_pin(pin); return 0; }
    luaL_argcheck(L, hz >= PWM_HZ_MIN && hz <= PWM_HZ_MAX, 2, "frequency 10..40000 Hz (0 stops)");
    lua_Number duty = luaL_checknumber(L, 3);
    luaL_argcheck(L, duty >= 0 && duty <= 1, 3, "duty 0..1");

    /* The timer: this pin's own if nothing else uses it, one already at
       this frequency, or a free one. */
    int timer = -1;
    if (ch >= 0 && s_pwm_timer[s_pwm[ch].timer].users == 1) timer = s_pwm[ch].timer;
    for (int i = 0; i < PWM_TIMERS; i++)
        if (s_pwm_timer[i].users && s_pwm_timer[i].hz == (uint32_t)hz && !(timer == i)) { timer = i; break; }
    for (int i = 0; timer < 0 && i < PWM_TIMERS; i++)
        if (!s_pwm_timer[i].users) timer = i;
    if (timer < 0) return luaL_error(L, "gpio.pwm: too many frequencies (%d at a time)", PWM_TIMERS);
    if (ch < 0) {
        for (int i = 0; ch < 0 && i < PWM_PINS; i++)
            if (!s_pwm[i].on) ch = i;
        if (ch < 0) return luaL_error(L, "gpio.pwm: too many pins (%d at a time)", PWM_PINS);
    }
    int bits = s_pwm_timer[timer].bits;
    if (s_pwm_timer[timer].hz != (uint32_t)hz || !s_pwm_timer[timer].users) {
        bits = (int)ledc_find_suitable_duty_resolution(PWM_CLK_HZ, (uint32_t)hz);
        if (bits > PWM_BITS) bits = PWM_BITS;
        ledc_timer_config_t tc = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = (ledc_timer_bit_t)bits,
                                   .timer_num = (ledc_timer_t)timer, .freq_hz = (uint32_t)hz,
                                   .clk_cfg = LEDC_USE_APB_CLK };
        if (!bits || ledc_timer_config(&tc) != ESP_OK) return luaL_error(L, "gpio.pwm: can't make %d Hz", (int)hz);
    }
    uint32_t raw = (uint32_t)(duty * (lua_Number)(1u << bits) + 0.5);
    if (s_pwm[ch].on && s_pwm[ch].timer == timer) {
        /* Only the duty (or its timer's frequency) changes: the pin keeps
           pulsing, nothing is set up again. */
        ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch, raw);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch);
    } else {
        ledc_channel_config_t cc = { .gpio_num = pin, .speed_mode = LEDC_LOW_SPEED_MODE,
                                     .channel = (ledc_channel_t)ch, .timer_sel = (ledc_timer_t)timer, .duty = raw,
                                     .hpoint = 0 };
        if (ledc_channel_config(&cc) != ESP_OK) return luaL_error(L, "gpio.pwm: GPIO%d can't be pulsed", pin);
    }
    if (s_pwm[ch].on) s_pwm_timer[s_pwm[ch].timer].users--;
    s_pwm_timer[timer].hz = (uint32_t)hz;
    s_pwm_timer[timer].bits = bits;
    s_pwm_timer[timer].users++;
    s_pwm[ch].on = true;
    s_pwm[ch].pin = pin;
    s_pwm[ch].timer = timer;
    s_pwm[ch].duty = duty;
    return 0;
}

static int l_gpio_mode(lua_State *L)
{
    int pin = gpio_pin_arg(L, 1);
    pwm_stop_pin(pin);
    int mode = ci(L, 2);
    gpio_config_t c = { .pin_bit_mask = 1ULL << pin };
    switch (mode) {
    case MODE_IN:          c.mode = GPIO_MODE_INPUT; break;
    case MODE_OUT:         c.mode = GPIO_MODE_INPUT_OUTPUT; break;
    case MODE_IN_PULLUP:   c.mode = GPIO_MODE_INPUT; c.pull_up_en = GPIO_PULLUP_ENABLE; break;
    case MODE_IN_PULLDOWN: c.mode = GPIO_MODE_INPUT; c.pull_down_en = GPIO_PULLDOWN_ENABLE; break;
    default: return luaL_argerror(L, 2, "use gpio.IN, gpio.OUT, gpio.IN_PULLUP or gpio.IN_PULLDOWN");
    }
    s_out &= ~(1ULL << pin);
    if (gpio_config(&c) != ESP_OK) return luaL_error(L, "GPIO%d does not support this mode", pin);
    if (mode == MODE_OUT) s_out |= 1ULL << pin;
    return 0;
}

/* Writing a pin that pulses stops the pulses: it's a plain output again,
   at `level` from the moment it drives. */
static void pwm_to_output(int pin, int level)
{
    if (pwm_find(pin) < 0) return;
    pwm_stop_pin(pin);
    gpio_set_level((gpio_num_t)pin, (uint32_t)level);
    gpio_config_t c = { .pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_INPUT_OUTPUT };
    gpio_config(&c);
    s_out |= 1ULL << pin;
}

/* The pin argument of gpio.write and gpio.toggle: an output or a pulsing pin. */
static int gpio_out_arg(lua_State *L, int idx)
{
    int pin = gpio_pin_arg(L, idx);
    if (!(s_out & (1ULL << pin)) && pwm_find(pin) < 0)
        luaL_error(L, "GPIO%d isn't an output: gpio.mode(%d, gpio.OUT) first", pin, pin);
    return pin;
}

static int l_gpio_write(lua_State *L)
{
    int pin = gpio_out_arg(L, 1);
    int v = lua_isboolean(L, 2) ? lua_toboolean(L, 2) : (luaL_checkinteger(L, 2) != 0);
    pwm_to_output(pin, v);
    gpio_set_level((gpio_num_t)pin, (uint32_t)v);
    return 0;
}

static int l_gpio_read(lua_State *L)
{
    lua_pushinteger(L, gpio_get_level((gpio_num_t)gpio_pin_arg(L, 1)));
    return 1;
}

static int l_gpio_toggle(lua_State *L)
{
    int pin = gpio_out_arg(L, 1);
    pwm_to_output(pin, 0);
    int v = !gpio_get_level((gpio_num_t)pin);
    gpio_set_level((gpio_num_t)pin, (uint32_t)v);
    lua_pushinteger(L, v);
    return 1;
}

static const luaL_Reg gpio_funcs[] = {
    { "mode", l_gpio_mode }, { "write", l_gpio_write }, { "read", l_gpio_read },
    { "toggle", l_gpio_toggle }, { "pwm", l_gpio_pwm }, { NULL, NULL },
};

/* ================================================================= sys */

static int sys_millis(lua_State *L) { lua_pushinteger(L, (lua_Integer)hw_millis64()); return 1; }

static int sys_delay(lua_State *L)
{
    lua_Integer ms = luaL_checkinteger(L, 1);
    luaL_argcheck(L, ms >= 0 && ms <= LUA_MAX_DELAY_MS, 1, "delay must be 0..1000 ms (use timers)");
    uint32_t t0 = hw_millis();
    for (;;) {                                        /* in slices: the touch is read meanwhile */
        uint32_t step = (uint32_t)ms - (hw_millis() - t0);
        if (step > TOUCH_READ_MS) step = TOUCH_READ_MS;
        vTaskDelay(pdMS_TO_TICKS(step) ? pdMS_TO_TICKS(step) : 1);
        buttons_sample();
        if (hw_millis() - t0 >= (uint32_t)ms) break;
    }
    script_watchdog_credit(L, t0, (uint32_t)ms);
    return 0;
}

static int sys_rate(lua_State *L)
{
    if (!lua_isnoneornil(L, 1)) script_set_rate(ci(L, 1));
    lua_pushinteger(L, script_get_rate());
    return 1;
}

static int sys_heap(lua_State *L)
{
    size_t used, peak, limit;
    script_heap_stats(&used, &peak, &limit);
    lua_pushinteger(L, (lua_Integer)used);
    lua_pushinteger(L, (lua_Integer)peak);
    lua_pushinteger(L, (lua_Integer)limit);
    return 3;
}

static int sys_battery(lua_State *L)
{
    int p = hw_battery_percent();
    if (p < 0) lua_pushnil(L); else lua_pushinteger(L, p);
    lua_pushboolean(L, hw_charging());
    lua_pushboolean(L, hw_usb_powered());
    return 3;
}

/* sys.port_power([on]) -> on: 5V out to the Grove ports. */
static int sys_port_power(lua_State *L)
{
    if (!lua_isnoneornil(L, 1)) {
        luaL_checktype(L, 1, LUA_TBOOLEAN);
        pin_power_set(lua_toboolean(L, 1));
    }
    lua_pushboolean(L, hw_port_power_on());
    return 1;
}

/* ============================================================== asset */

/* asset(name): a file of the script's own app (a picture, a sound), as a
   string; nil and why not if it isn't there. Not other apps' files. */
static int l_asset(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    luaL_argcheck(L, !strchr(name, '/'), 1, "a file of this app: a name without '/'");
    char app[VFS_APP_NAME_MAX], path[VFS_MAX_NAME];
    script_app(app, sizeof app);
    snprintf(path, sizeof path, "%s/%s", app, name);
    size_t len = 0;
    uint8_t *data = app[0] && vfs_valid_path(path) ? vfs_load(path, &len) : NULL;
    if (!data) {
        lua_pushnil(L);
        lua_pushfstring(L, "no such file in the app: %s", name);
        return 2;
    }
    lua_pushlstring(L, (const char *)data, len);
    free(data);
    return 1;
}

/* The board's own id: 12 hex digits from its factory MAC address. Hashed,
   so an app that sends it somewhere doesn't give out the address its
   Wi-Fi uses. The same on every boot and after a firmware update. */
static const char *board_id(void)
{
    static char id[13];
    if (!id[0]) {
        uint8_t mac[6] = { 0 }, h[32];
        esp_efuse_mac_get_default(mac);
        uint8_t in[8 + 6];
        memcpy(in, "luaboard", 8);
        memcpy(in + 8, mac, 6);
        lb_sha256(in, sizeof in, h);
        for (int i = 0; i < 6; i++) snprintf(id + 2 * i, 3, "%02x", h[i]);
    }
    return id;
}

static int sys_info(lua_State *L)
{
    lua_newtable(L);
    lua_pushstring(L, FW_NAME); lua_setfield(L, -2, "fw");
    lua_pushstring(L, fw_version()); lua_setfield(L, -2, "version");
    lua_pushstring(L, BOARD_NAME); lua_setfield(L, -2, "board");
    lua_pushstring(L, hw_model()); lua_setfield(L, -2, "model");
    lua_pushstring(L, LUA_RELEASE); lua_setfield(L, -2, "lua");
    lua_pushinteger(L, APP_LUA_API); lua_setfield(L, -2, "lua_api");
    lua_pushstring(L, script_current()); lua_setfield(L, -2, "script");
    char app[32];
    script_app(app, sizeof app);
    lua_pushstring(L, app); lua_setfield(L, -2, "app");
    lua_pushstring(L, power_boot_reason()); lua_setfield(L, -2, "boot");
    lua_pushstring(L, board_id()); lua_setfield(L, -2, "id");
    return 1;
}

/* The zone argument at idx: the device's (none or nil), or a POSIX TZ
   string ("UTC" for UTC). A string that isn't one is an error. */
static const tz_t *zone_arg(lua_State *L, int idx, tz_t *buf)
{
    if (lua_isnoneornil(L, idx)) return clock_zone();
    const char *s = luaL_checkstring(L, idx);
    if (strlen(s) >= TZ_MAX || !tz_parse(s, buf))
        luaL_argerror(L, idx, lua_pushfstring(L, "not a time zone: %s (a POSIX TZ string such as \"UTC\" or "
                                                 "\"EST5EDT,M3.2.0,M11.1.0\")", s));
    return buf;
}

/* An integer field of sys.time's table: `def` if absent (-1: required). */
static int64_t time_field(lua_State *L, const char *k, int64_t def)
{
    int t = lua_getfield(L, 1, k);
    int ok;
    lua_Integer v = lua_tointegerx(L, -1, &ok);
    lua_pop(L, 1);
    if (t == LUA_TNIL && def >= 0) return def;
    if (!ok) luaL_error(L, "sys.time: field '%s' must be a whole number", k);
    return (int64_t)v;
}

/* sys.time([x], [zone]) -> UTC seconds since 1970.
     no x: now, or nil until the time is known (NTP or the RTC chip);
     a table {year, month, day, [hour], [min], [sec]}: that local time in
       the zone (fields out of range carry over, as os.time's);
     a string (RFC 3339 / ISO 8601, an HTTP date): that time, in the zone
       if it has no offset; nil, err if it isn't one. */
static int sys_time(lua_State *L)
{
    if (lua_isnoneornil(L, 1)) {
        int64_t t = clock_now();
        if (t == 0) lua_pushnil(L); else lua_pushinteger(L, (lua_Integer)t);
        return 1;
    }
    tz_t buf;
    const tz_t *zone = zone_arg(L, 2, &buf);
    int64_t t;
    if (lua_type(L, 1) == LUA_TSTRING) {
        size_t len;
        const char *s = lua_tolstring(L, 1, &len);
        if (!tz_parse_time(s, len, zone, &t)) {
            lua_pushnil(L);
            lua_pushfstring(L, "not a date and time: %s", len > 64 ? "(too long)" : s);
            return 2;
        }
        lua_pushinteger(L, (lua_Integer)t);
        return 1;
    }
    luaL_argexpected(L, lua_istable(L, 1), 1, "table, string or nil");
    int64_t local;
    if (!tz_fields(time_field(L, "year", -1), time_field(L, "month", -1), time_field(L, "day", -1),
                   time_field(L, "hour", 0), time_field(L, "min", 0), time_field(L, "sec", 0), &local))
        return luaL_error(L, "sys.time: time out of range (years 0..9999)");
    t = tz_from_local(zone, local);
    if (t < TZ_MIN_TIME || t > TZ_MAX_TIME) return luaL_error(L, "sys.time: time out of range (years 0..9999)");
    lua_pushinteger(L, (lua_Integer)t);
    return 1;
}

/* sys.date([t], [zone]) -> { year, month, day, hour, min, sec, wday, yday,
   isdst, offset, zone } in the device's time zone or `zone`; nil if t is
   left out and the time isn't known yet. wday: 1 = Sunday, like os.date. */
/* sys.date([t], [zone]) -> table, or sys.date(format, [t], [zone]) -> text. */
static int sys_date(lua_State *L)
{
    int a = lua_type(L, 1) == LUA_TSTRING ? 2 : 1;     /* where t is */
    int64_t t = lua_isnoneornil(L, a) ? clock_now() : (int64_t)luaL_checkinteger(L, a);
    tz_t buf;
    const tz_t *zone = zone_arg(L, a + 1, &buf);
    if (lua_isnoneornil(L, a) && t == 0) { lua_pushnil(L); return 1; }
    luaL_argcheck(L, t >= TZ_MIN_TIME && t <= TZ_MAX_TIME, a, "time out of range (years 0..9999)");
    tz_tm_t tm;
    tz_local(zone, t, &tm);
    if (a == 2) {
        char out[256];
        int n = tz_format(out, sizeof out, lua_tostring(L, 1), &tm);
        if (n == TZ_FORMAT_BAD)
            return luaL_argerror(L, 1, "a % the format doesn't know (%Y %y %m %d %H %I %M %S %j %p %a %A %b %B "
                                       "%Z %z %%, %-d for no leading zero)");
        if (n == TZ_FORMAT_LONG) return luaL_argerror(L, 1, "the text would be over 255 bytes");
        lua_pushlstring(L, out, (size_t)n);
        return 1;
    }
    lua_createtable(L, 0, 11);
    lua_pushinteger(L, (lua_Integer)tm.year); lua_setfield(L, -2, "year");
    lua_pushinteger(L, tm.month); lua_setfield(L, -2, "month");
    lua_pushinteger(L, tm.day); lua_setfield(L, -2, "day");
    lua_pushinteger(L, tm.hour); lua_setfield(L, -2, "hour");
    lua_pushinteger(L, tm.min); lua_setfield(L, -2, "min");
    lua_pushinteger(L, tm.sec); lua_setfield(L, -2, "sec");
    lua_pushinteger(L, tm.wday + 1); lua_setfield(L, -2, "wday");
    lua_pushinteger(L, tm.yday + 1); lua_setfield(L, -2, "yday");
    lua_pushboolean(L, tm.dst); lua_setfield(L, -2, "isdst");
    lua_pushinteger(L, tm.offset); lua_setfield(L, -2, "offset");
    lua_pushstring(L, tm.zone); lua_setfield(L, -2, "zone");
    return 1;
}

static int sys_timezone(lua_State *L)
{
    lua_pushstring(L, clock_tz());
    return 1;
}

/* sys.off([seconds]): turn the device off once this call has returned.
   With seconds, it turns itself on again in time for the app to run by
   then (early, never late). */
static int sys_off(lua_State *L)
{
    uint32_t seconds = 0;
    if (!lua_isnoneornil(L, 1)) {
        lua_Number n = luaL_checknumber(L, 1);
        if (!(n >= OFF_MIN_S)) return luaL_error(L, "sys.off: at least %d s", OFF_MIN_S);
        if (n > OFF_MAX_S) return luaL_error(L, "sys.off: at most %d days", (int)(OFF_MAX_S / 86400));
        seconds = (uint32_t)n;
    }
    if (power_off_request(seconds, "app") == WAKE_NEEDS_CLOCK)
        return luaL_error(L, "sys.off: needs the time for more than 255 minutes");
    return script_leave(L, false);      /* nothing more of the app runs; power_poll turns it off */
}

/* sys.keep_on([on]) -> on. While on, the Turn off after setting doesn't turn
   the device off. Off again when the script ends. */
static int sys_keep_on(lua_State *L)
{
    if (!lua_isnoneornil(L, 1)) power_keep_on(lua_api_optbool(L, 1, false));
    lua_pushboolean(L, power_kept_on());
    return 1;
}

/* sys.exit(): leave the app at once, back to the home menu. */
static int sys_exit(lua_State *L)
{
    return script_leave(L, true);
}

/* sys._error(msg): raise msg at the app's line that led here, past the
   built-in Lua libraries' own frames (prelude, http.lua, ...), so their
   checks point at the app's call as a C function's luaL_error does. */
static int sys_error(lua_State *L)
{
    const char *msg = luaL_checkstring(L, 1);
    lua_Debug ar;
    lua_pushliteral(L, "");      /* no app frame left (a tail call from it): no position */
    for (int i = 1; lua_getstack(L, i, &ar); i++) {
        lua_getinfo(L, "S", &ar);
        if (*ar.what == 'C') continue;
        if (ar.source[0] == '=' && builtin_lua(ar.source + 1, NULL)) continue;
        lua_pop(L, 1);
        luaL_where(L, i);
        break;
    }
    lua_pushstring(L, msg);
    lua_concat(L, 2);
    return lua_error(L);
}

/* sys._watchdog_left(): ms left before the watchdog stops this call. The
   built-in libraries use it to yield in a long job (json.decode of a big
   body) while there is time left. */
static int sys_watchdog_left(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)script_watchdog_left());
    return 1;
}

static const luaL_Reg sys_funcs[] = {
    { "millis", sys_millis }, { "delay", sys_delay }, { "rate", sys_rate },
    { "heap", sys_heap }, { "battery", sys_battery }, { "port_power", sys_port_power }, { "info", sys_info },
    { "exit", sys_exit }, { "off", sys_off }, { "_error", sys_error }, { "_watchdog_left", sys_watchdog_left }, { "keep_on", sys_keep_on }, { "time", sys_time }, { "date", sys_date }, { "timezone", sys_timezone },
    { NULL, NULL },
};

/* ============================================================ register */

static void set_int(lua_State *L, const char *k, lua_Integer v)
{
    lua_pushinteger(L, v);
    lua_setfield(L, -2, k);
}

void lua_api_release_hw(void)
{
    /* GPIO back to the state of a free pin (not driven, weakly pulled up):
       nothing stays driven after the script that drove it is gone. A relay
       or motor on a pin goes off if it has the pull-down the docs ask for. */
    for (int i = 0; i < PWM_PINS; i++)
        if (s_pwm[i].on) pwm_stop_pin(s_pwm[i].pin);
    for (int pin = 0; pin < 64; pin++)
        if (pin_user(pin) == PIN_GPIO) hw_gpio_release(pin);
    pin_give_back(PIN_GPIO);
    s_out = 0;
    hw_backlight(settings_brightness());
    s_level = -1;
    sleep_keep_awake(false);
    power_keep_on(false);
    pin_power_reset();
}

void lua_api_register(lua_State *L)
{
    s_drew = false;                       /* new VM, new script */
    luaL_newlib(L, disp_funcs);
    set_int(L, "W", BOARD_LCD_W);
    set_int(L, "H", BOARD_LCD_H);
    static const struct { const char *name; uint32_t rgb; } colors[] = {
        { "BLACK", 0x000000 }, { "WHITE", 0xFFFFFF }, { "GRAY", 0x808080 },
        { "DARKGRAY", 0x303030 }, { "RED", 0xFF0000 }, { "GREEN", 0x00FF00 },
        { "BLUE", 0x0000FF }, { "YELLOW", 0xFFFF00 }, { "CYAN", 0x00FFFF },
        { "MAGENTA", 0xFF00FF }, { "ORANGE", 0xFF8000 }, { "NAVY", 0x000080 },
    };
    for (size_t i = 0; i < sizeof colors / sizeof colors[0]; i++) set_int(L, colors[i].name, colors[i].rgb);
    lua_setglobal(L, "disp");

    luaL_newlib(L, touch_funcs);
    set_int(L, "W", BOARD_TOUCH_W);
    set_int(L, "H", BOARD_TOUCH_H);
    lua_setglobal(L, "touch");

    luaL_newlib(L, adc_funcs);
    set_int(L, "PORTB_IN", BOARD_PORTB_IN);             /* pins, as in gpio */
    set_int(L, "PORTB_OUT", BOARD_PORTB_OUT);
    set_int(L, "PORTA_SDA", BOARD_PORTA_SDA);
    set_int(L, "PORTA_SCL", BOARD_PORTA_SCL);
    set_int(L, "MAX", 4095);
    lua_setglobal(L, "adc");

    luaL_newlib(L, gpio_funcs);
    set_int(L, "IN", MODE_IN);
    set_int(L, "OUT", MODE_OUT);
    set_int(L, "IN_PULLUP", MODE_IN_PULLUP);
    set_int(L, "IN_PULLDOWN", MODE_IN_PULLDOWN);
    set_int(L, "PORTA_SDA", BOARD_PORTA_SDA);
    set_int(L, "PORTA_SCL", BOARD_PORTA_SCL);
    set_int(L, "PORTB_IN", BOARD_PORTB_IN);
    set_int(L, "PORTB_OUT", BOARD_PORTB_OUT);
    set_int(L, "PORTC_RX", BOARD_PORTC_RX);
    set_int(L, "PORTC_TX", BOARD_PORTC_TX);
    lua_setglobal(L, "gpio");

    luaL_newlib(L, sys_funcs);
    lua_setglobal(L, "sys");

    lua_register(L, "asset", l_asset);
}
