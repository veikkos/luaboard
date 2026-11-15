/*
 * Hardware API exposed to Lua scripts: disp, adc, gpio, sys.
 * Reference: docs/LUA_API.md.
 */
#pragma once

#include <stdbool.h>

#include "lua.h"

void lua_api_register(lua_State *L);
/** Undo hardware changes a script made: GPIO it used, display brightness. */
void lua_api_release_hw(void);
/** A script has a pin pulsing (gpio.pwm): the PWM clock must run (no light sleep). */
bool lua_api_pwm_running(void);
/** Has the script called a drawing function since its VM was created? */
bool lua_api_drew(void);
