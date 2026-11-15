/* Lua `imu` and `light` modules: the board's own sensors (see lua_sensors.c). */
#pragma once

#include "lua.h"

void lua_sensors_register(lua_State *L);
/** The script ended: the sensors it used are switched off. */
void lua_sensors_release(void);
