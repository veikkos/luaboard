/* Lua `net` module (non-blocking TCP sockets). See lua_net.c. */
#pragma once

#include <stdbool.h>
#include "lua.h"

void lua_net_register(lua_State *L);
/** Close every socket opened by scripts (VM reset). */
void lua_net_close_all(void);
struct sockaddr_in;
/** True if `peer` is the local end of a socket a script opened: a script on
    this device connecting to the device's own protocol port. */
bool lua_net_is_own_socket(const struct sockaddr_in *peer);
