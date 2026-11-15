/*
 * Lua `i2c` module: the I2C bus on Grove Port A (G2 SDA, G1 SCL), for add-on
 * sensors and other units. Only Port A: the internal bus carries the power
 * chip, touch and clock, which scripts must not reach.
 *
 *   i2c.scan() -> { addr, ... }                 addresses that answer
 *   i2c.write(addr, data) -> true | nil, err
 *   i2c.read(addr, n) -> data | nil, err
 *   i2c.write_read(addr, data, n) -> data | nil, err   (repeated START)
 *   i2c.speed([hz]) -> hz                        10000..400000, default 100000
 *
 * The bus is set up on first use and released when the script ends; while
 * it's in use, gpio and adc can't take the Port A pins.
 */
#include "lua_i2c.h"

#include <stdio.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "lauxlib.h"
#include "hw.h"
#include "lua_pins.h"

#define PORTA_I2C   I2C_NUM_1
#define XFER_MS     50
#define PROBE_MS    10
#define MAX_XFER    1024
#define DEFAULT_HZ  100000

static i2c_master_bus_handle_t s_bus;
static uint32_t s_hz = DEFAULT_HZ;

void lua_i2c_release(void)
{
    if (!s_bus) return;
    i2c_del_master_bus(s_bus);
    s_bus = NULL;
    hw_gpio_release(BOARD_PORTA_SDA);
    hw_gpio_release(BOARD_PORTA_SCL);
    pin_give_back(PIN_I2C);
    s_hz = DEFAULT_HZ;
}

static void open_bus(lua_State *L)
{
    if (s_bus) return;
    pin_check(L, BOARD_PORTA_SDA, PIN_I2C);
    pin_check(L, BOARD_PORTA_SCL, PIN_I2C);
    i2c_master_bus_config_t c = {
        .i2c_port = PORTA_I2C,
        .sda_io_num = BOARD_PORTA_SDA,
        .scl_io_num = BOARD_PORTA_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,   /* most units have their own as well */
    };
    if (i2c_new_master_bus(&c, &s_bus) != ESP_OK) {
        s_bus = NULL;
        luaL_error(L, "can't set up I2C on Port A");
    }
    pin_take(BOARD_PORTA_SDA, PIN_I2C);
    pin_take(BOARD_PORTA_SCL, PIN_I2C);
}

static int addr_arg(lua_State *L, int idx)
{
    lua_Integer a = luaL_checkinteger(L, idx);
    luaL_argcheck(L, a >= 0x08 && a <= 0x77, idx, "7-bit I2C address 0x08..0x77");
    return (int)a;
}

/* One transaction with the device at addr: write w (if any), then read r
   (if any) after a repeated START. */
static esp_err_t xfer(int addr, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = (uint16_t)addr,
                               .scl_speed_hz = s_hz };
    i2c_master_dev_handle_t d;
    esp_err_t err = i2c_master_bus_add_device(s_bus, &dc, &d);
    if (err != ESP_OK) return err;
    if (wn && rn) err = i2c_master_transmit_receive(d, w, wn, r, rn, XFER_MS);
    else if (wn) err = i2c_master_transmit(d, w, wn, XFER_MS);
    else err = i2c_master_receive(d, r, rn, XFER_MS);
    i2c_master_bus_rm_device(d);
    return err;
}

static int fail(lua_State *L, int addr, esp_err_t err)
{
    char msg[80];
    if (err == ESP_FAIL || err == ESP_ERR_NOT_FOUND) snprintf(msg, sizeof msg, "no answer from 0x%02X", addr);
    else if (err == ESP_ERR_TIMEOUT) snprintf(msg, sizeof msg, "0x%02X timed out (bus stuck?)", addr);
    else snprintf(msg, sizeof msg, "I2C error with 0x%02X (%s)", addr, esp_err_to_name(err));
    lua_pushnil(L);
    lua_pushstring(L, msg);
    return 2;
}

static int l_scan(lua_State *L)
{
    open_bus(L);
    lua_newtable(L);
    lua_Integer n = 0;
    for (int a = 0x08; a <= 0x77; a++) {
        esp_err_t err = i2c_master_probe(s_bus, (uint16_t)a, PROBE_MS);
        if (err == ESP_OK) {
            lua_pushinteger(L, a);
            lua_rawseti(L, -2, ++n);
        } else if (err == ESP_ERR_TIMEOUT) {          /* SDA or SCL held low: every probe would wait */
            lua_pushnil(L);
            lua_pushliteral(L, "Port A bus stuck (no pull-ups, or a unit holding a line low?)");
            return 2;
        }
    }
    return 1;
}

static int l_write(lua_State *L)
{
    int addr = addr_arg(L, 1);
    size_t n;
    const char *data = luaL_checklstring(L, 2, &n);
    luaL_argcheck(L, n >= 1 && n <= MAX_XFER, 2, "1..1024 bytes");
    open_bus(L);
    esp_err_t err = xfer(addr, (const uint8_t *)data, n, NULL, 0);
    if (err != ESP_OK) return fail(L, addr, err);
    lua_pushboolean(L, 1);
    return 1;
}

static int read_common(lua_State *L, int addr, const char *w, size_t wn, int nidx)
{
    lua_Integer n = luaL_checkinteger(L, nidx);
    luaL_argcheck(L, n >= 1 && n <= MAX_XFER, nidx, "1..1024 bytes");
    open_bus(L);
    luaL_Buffer b;
    uint8_t *r = (uint8_t *)luaL_buffinitsize(L, &b, (size_t)n);
    esp_err_t err = xfer(addr, (const uint8_t *)w, wn, r, (size_t)n);
    if (err != ESP_OK) return fail(L, addr, err);
    luaL_pushresultsize(&b, (size_t)n);
    return 1;
}

static int l_read(lua_State *L)
{
    return read_common(L, addr_arg(L, 1), NULL, 0, 2);
}

static int l_write_read(lua_State *L)
{
    int addr = addr_arg(L, 1);
    size_t wn;
    const char *w = luaL_checklstring(L, 2, &wn);
    luaL_argcheck(L, wn >= 1 && wn <= MAX_XFER, 2, "1..1024 bytes");
    return read_common(L, addr, w, wn, 3);
}

static int l_speed(lua_State *L)
{
    if (!lua_isnoneornil(L, 1)) {
        lua_Integer hz = luaL_checkinteger(L, 1);
        luaL_argcheck(L, hz >= 10000 && hz <= 400000, 1, "10000..400000 Hz");
        s_hz = (uint32_t)hz;
    }
    lua_pushinteger(L, (lua_Integer)s_hz);
    return 1;
}

void lua_i2c_register(lua_State *L)
{
    static const luaL_Reg fns[] = {
        { "scan", l_scan }, { "write", l_write }, { "read", l_read }, { "write_read", l_write_read },
        { "speed", l_speed }, { NULL, NULL },
    };
    luaL_newlib(L, fns);
    lua_setglobal(L, "i2c");
}
