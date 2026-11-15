/* Lua `sd` module: files on the microSD card (see lua_sd.c). */
#pragma once

#include "lua.h"

void lua_sd_register(lua_State *L);
/** The script ended: let go of the card. */
void lua_sd_release(void);
