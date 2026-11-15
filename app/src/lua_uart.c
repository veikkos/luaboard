/*
 * Lua `uart` module: one serial port for add-on units (GPS, serial sensors,
 * another board), on Grove Port C unless other pins are given.
 *
 *   uart.open(baud, [opts]) -> true        opts: rx, tx (pins), bits (5..8),
 *                                          parity ("none"|"even"|"odd"), stop (1|2)
 *   uart.write(data) -> n                  bytes taken (up to about 1000 at a time, fewer when the output buffer fills)
 *   uart.read([max]) -> data               what has arrived ("" if nothing), up to max bytes
 *   uart.line() -> line | nil              a whole line that has arrived, without its line end
 *   uart.close()
 *
 * Nothing blocks, except closing: it waits up to 0.5 s for what's queued to
 * be sent. The port is closed when the script ends; while it's open,
 * gpio and adc can't take its pins.
 */
#include "lua_uart.h"

#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "hw.h"
#include "lauxlib.h"
#include "lua_pins.h"

#define PORT       UART_NUM_1
#define RX_BUF     2048           /* arrives while the script isn't reading: more than this is lost */
#define TX_BUF     2048
#define READ_MAX   4096
#define UART_LINE_MAX   1024           /* a longer line comes in pieces of this */
#define BAUD_MIN   300
#define BAUD_MAX   2000000
#define DRAIN_MS   500            /* closing waits this long at most for queued bytes to leave */

static bool s_open;
static int s_rx, s_tx;
static char s_ahead[UART_LINE_MAX];    /* read from the driver, not yet given to the script (uart.line) */
static size_t s_ahead_len;

bool lua_uart_is_open(void) { return s_open; }

void lua_uart_release(void)
{
    if (!s_open) return;
    /* What was written is sent before the driver goes, so a command
       written just before closing (or reopening at another rate) isn't
       lost. Bounded: at slow rates a full buffer takes seconds. */
    uart_wait_tx_done(PORT, pdMS_TO_TICKS(DRAIN_MS));
    uart_driver_delete(PORT);
    hw_gpio_release(s_rx);
    hw_gpio_release(s_tx);
    pin_give_back(PIN_UART);
    s_open = false;
    s_ahead_len = 0;
}

static void need_open(lua_State *L)
{
    if (!s_open) luaL_error(L, "uart isn't open (uart.open(baud) first)");
}

/* A pin from the options table (or its default) that the port may take. */
static int pin_opt(lua_State *L, const char *key, int dflt)
{
    int pin = dflt;
    if (lua_istable(L, 2)) {
        if (lua_getfield(L, 2, key) != LUA_TNIL) pin = (int)luaL_checkinteger(L, -1);
        lua_pop(L, 1);
    }
    if (!GPIO_IS_VALID_GPIO(pin)) luaL_error(L, "uart: no GPIO%d", pin);
    if (hw_gpio_is_reserved(pin)) luaL_error(L, "GPIO%d is reserved by the firmware", pin);
    pin_check(L, pin, PIN_UART);
    return pin;
}

static int int_opt(lua_State *L, const char *key, int dflt, int lo, int hi)
{
    int v = dflt;
    if (lua_istable(L, 2)) {
        if (lua_getfield(L, 2, key) != LUA_TNIL) v = (int)luaL_checkinteger(L, -1);
        lua_pop(L, 1);
    }
    if (v < lo || v > hi) luaL_error(L, "uart: %s must be %d..%d", key, lo, hi);
    return v;
}

static int l_open(lua_State *L)
{
    lua_Integer baud = luaL_checkinteger(L, 1);
    luaL_argcheck(L, baud >= BAUD_MIN && baud <= BAUD_MAX, 1, "baud rate 300..2000000");
    if (!lua_isnoneornil(L, 2)) luaL_checktype(L, 2, LUA_TTABLE);
    int rx = pin_opt(L, "rx", BOARD_PORTC_RX), tx = pin_opt(L, "tx", BOARD_PORTC_TX);
    if (rx == tx) return luaL_error(L, "uart: rx and tx are the same pin");
    int bits = int_opt(L, "bits", 8, 5, 8), stop = int_opt(L, "stop", 1, 1, 2);
    uart_parity_t parity = UART_PARITY_DISABLE;
    if (lua_istable(L, 2)) {
        if (lua_getfield(L, 2, "parity") != LUA_TNIL) {
            static const char *const names[] = { "none", "even", "odd", NULL };
            static const uart_parity_t values[] = { UART_PARITY_DISABLE, UART_PARITY_EVEN, UART_PARITY_ODD };
            parity = values[luaL_checkoption(L, -1, NULL, names)];
        }
        lua_pop(L, 1);
    }
    lua_uart_release();                               /* open again: with the new settings (all checked by now) */
    uart_config_t cfg = {
        .baud_rate = (int)baud,
        .data_bits = (uart_word_length_t)(UART_DATA_5_BITS + (bits - 5)),
        .parity = parity,
        .stop_bits = stop == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(PORT, RX_BUF, TX_BUF, 0, NULL, 0) != ESP_OK)
        return luaL_error(L, "can't set up the serial port");
    if (uart_param_config(PORT, &cfg) != ESP_OK ||
        uart_set_pin(PORT, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        uart_driver_delete(PORT);
        return luaL_error(L, "can't set up the serial port");
    }
    s_rx = rx;
    s_tx = tx;
    pin_take(rx, PIN_UART);
    pin_take(tx, PIN_UART);
    s_open = true;
    s_ahead_len = 0;
    lua_pushboolean(L, 1);
    return 1;
}

static int l_write(lua_State *L)
{
    size_t len;
    const char *data = luaL_checklstring(L, 1, &len);
    need_open(L);
    /* Never waits for the line: only what fits now. The driver's buffer is
       a ring whose free room can be in two pieces, and a write bigger than
       a piece would wait for the line inside the driver; half the room
       always fits in one. */
    size_t room = 0;
    uart_get_tx_buffer_free_size(PORT, &room);
    if (len > room / 2) len = room / 2;
    int n = len ? uart_write_bytes(PORT, data, len) : 0;
    lua_pushinteger(L, n > 0 ? n : 0);
    return 1;
}

static int l_read(lua_State *L)
{
    lua_Integer max = luaL_optinteger(L, 1, 1024);
    luaL_argcheck(L, max >= 1 && max <= READ_MAX, 1, "1..4096 bytes");
    need_open(L);
    luaL_Buffer b;
    char *buf = luaL_buffinitsize(L, &b, (size_t)max);
    size_t n = s_ahead_len < (size_t)max ? s_ahead_len : (size_t)max;   /* first what uart.line() read ahead */
    memcpy(buf, s_ahead, n);
    memmove(s_ahead, s_ahead + n, s_ahead_len - n);
    s_ahead_len -= n;
    if (n < (size_t)max) {
        int got = uart_read_bytes(PORT, buf + n, (uint32_t)((size_t)max - n), 0);
        if (got > 0) n += (size_t)got;
    }
    luaL_pushresultsize(&b, n);
    return 1;
}

static int l_line(lua_State *L)
{
    need_open(L);
    if (s_ahead_len < UART_LINE_MAX) {
        int got = uart_read_bytes(PORT, s_ahead + s_ahead_len, (uint32_t)(UART_LINE_MAX - s_ahead_len), 0);
        if (got > 0) s_ahead_len += (size_t)got;
    }
    char *nl = memchr(s_ahead, '\n', s_ahead_len);
    if (!nl && s_ahead_len < UART_LINE_MAX) { lua_pushnil(L); return 1; }
    size_t len = nl ? (size_t)(nl - s_ahead) : s_ahead_len, used = nl ? len + 1 : len;
    if (nl && len && s_ahead[len - 1] == '\r') len--;
    lua_pushlstring(L, s_ahead, len);
    memmove(s_ahead, s_ahead + used, s_ahead_len - used);
    s_ahead_len -= used;
    return 1;
}

static int l_close(lua_State *L)
{
    (void)L;
    lua_uart_release();
    return 0;
}

static const luaL_Reg uart_funcs[] = {
    { "open", l_open }, { "write", l_write }, { "read", l_read }, { "line", l_line }, { "close", l_close },
    { NULL, NULL },
};

void lua_uart_register(lua_State *L)
{
    luaL_newlib(L, uart_funcs);
    lua_setglobal(L, "uart");
}
