#include "lua_pins.h"

#include "hw.h"
#include "lauxlib.h"

#define PINS 64

static unsigned char s_user[PINS];   /* pin_user_t */
static bool s_power_set;             /* the script switched the ports' 5V itself */

static const char *user_name(pin_user_t u)
{
    return u == PIN_GPIO ? "gpio or adc" : u == PIN_I2C ? "i2c (Port A)" : u == PIN_UART ? "uart" : "nothing";
}

pin_user_t pin_user(int pin) { return pin >= 0 && pin < PINS ? (pin_user_t)s_user[pin] : PIN_FREE; }

void pin_check(lua_State *L, int pin, pin_user_t who)
{
    pin_user_t u = pin_user(pin);
    if (u != PIN_FREE && u != who) luaL_error(L, "GPIO%d is in use by %s in this script", pin, user_name(u));
}

void pin_take(int pin, pin_user_t who)
{
    if (pin >= 0 && pin < PINS) s_user[pin] = (unsigned char)who;
    if (!s_power_set) hw_port_power(true);
}

void pin_power_set(bool on)
{
    s_power_set = true;
    hw_port_power(on);
}

void pin_power_reset(void)
{
    s_power_set = false;
    hw_port_power(false);
}

void pin_give_back(pin_user_t who)
{
    for (int pin = 0; pin < PINS; pin++)
        if (s_user[pin] == who) s_user[pin] = PIN_FREE;
}
