/* Lua module camera: the board's camera (CoreS3 and Lite). */
#pragma once

#include "lua.h"

void lua_camera_register(lua_State *L);
/** The camera off, at a script's end. */
void lua_camera_release(void);
