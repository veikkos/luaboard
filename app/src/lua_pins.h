/* Which part of the Lua API has each pin a script uses, so two never drive
   one pin: gpio, adc and gpio.pwm (lua_api.c), i2c on Port A (lua_i2c.c)
   and uart (lua_uart.c). A module that takes pins checks and takes them
   here, and gives them back when it lets them go (uart.close, the end of
   the script); the pins' hardware state it restores itself. */
#pragma once

#include <stdbool.h>

#include "lua.h"

typedef enum { PIN_FREE, PIN_GPIO, PIN_I2C, PIN_UART } pin_user_t;

/** Raise a Lua error if another user has the pin (free, or already who's: fine). */
void pin_check(lua_State *L, int pin, pin_user_t who);
/** The pin is who's now (after pin_check). */
void pin_take(int pin, pin_user_t who);
/** Every pin who has is free again. */
void pin_give_back(pin_user_t who);
pin_user_t pin_user(int pin);

/* 5V out to the ports follows the pins: off until the script takes one (a
   unit on a port then has its power), and off again when the script ends.
   Unless the script switches it itself (sys.port_power): that stands. */
/** The script switched the power itself. */
void pin_power_set(bool on);
/** The script ended: off, for the next one to start from. */
void pin_power_reset(void);
