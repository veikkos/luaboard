/* Lua sources built into the firmware: the .lua files in app/lua, embedded with #embed. */
#pragma once

#include <stddef.h>

/** Built-in source for "prelude" or a library file name ("mqtt.lua"), or NULL. */
const char *builtin_lua(const char *name, size_t *len);
