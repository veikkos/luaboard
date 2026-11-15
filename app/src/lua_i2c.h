/* Lua `i2c` module: Grove Port A (see lua_i2c.c). */
#pragma once

#include <stdbool.h>

#include "lua.h"

void lua_i2c_register(lua_State *L);
/** The script ended: free the bus and its pins. */
void lua_i2c_release(void);
