/* Lua `store` module: per-app values kept in flash (see lua_store.c). */
#pragma once

#include "lua.h"

void lua_store_register(lua_State *L);
