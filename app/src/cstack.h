/*
 * Guard for the C stack under Lua.
 *
 * Lua stops runaway nesting by counting nested C calls (LUAI_MAXCCALLS), but
 * what one level costs varies a lot: a pcall takes a few hundred bytes of C
 * stack, a string.gsub callback over 2 KB. A count low enough for the worst
 * case cuts ordinary scripts short, and one that suits ordinary scripts lets
 * the worst case run off the end of the stack and crash the device.
 *
 * So the Lua core is compiled with this header forced in (-include), which
 * makes LUAI_MAXCCALLS a call to cstack_limit(). Lua evaluates it on every
 * nested C call and checks the answer against its count, and the answer
 * depends on how much of the main task's stack is left:
 *   - more than CSTACK_SOFT: Lua's usual count limit
 *   - less: the count right now, so Lua raises "C stack overflow" here, as
 *     at its own limit; the error's message handler may then still run
 *   - less than CSTACK_HARD: Lua gives up on that error ("error in error
 *     handling") instead of going deeper
 * Lua itself is unchanged; LUAI_MAXCCALLS is one of its configuration macros.
 */
#pragma once

#include <stddef.h>

/** Call first thing in the main task: its stack is `size` bytes (the
 *  device's CONFIG_ESP_MAIN_TASK_STACK_SIZE, the same in the simulator). */
void cstack_init(size_t size);

/** LUAI_MAXCCALLS for the Lua core, given the current count of nested C calls. */
unsigned cstack_limit(unsigned ccalls);

#ifndef LUAI_MAXCCALLS
#define LUAI_MAXCCALLS cstack_limit(getCcalls(L))
#endif
