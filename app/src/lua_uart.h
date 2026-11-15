/* Lua `uart` module: a serial port, on Grove Port C by default (see lua_uart.c). */
#pragma once

#include <stdbool.h>

#include "lua.h"

void lua_uart_register(lua_State *L);
/** The script ended: close the port and free its pins. */
void lua_uart_release(void);
/** The port is open: bytes may arrive at any time (no light sleep). */
bool lua_uart_is_open(void);
