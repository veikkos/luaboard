/* Guard for the C stack under Lua, see cstack.h. */
#include "cstack.h"

#include <stdint.h>

/* Lua's own default; with the stack guard the count rarely matters. */
#define CSTACK_MAX_CCALLS 200u
/* Room kept free below the deepest Lua call for C code that doesn't call
   back into Lua: a TLS handshake step, the firmware's drawing and file code. */
#define CSTACK_SOFT (16 * 1024)
/* Room kept for the error handler (the traceback) once the error is raised. */
#define CSTACK_HARD (8 * 1024)

static uintptr_t s_top, s_bottom;
/* The nesting count at which the overflow error was raised (0: none since the
   stack was last fine). Lua evaluates the limit more than once per check, so
   the answer has to stay the same at that count; deeper calls are the error's
   handler running. */
static unsigned s_err_at;

void cstack_init(size_t size)
{
    s_top = (uintptr_t)__builtin_frame_address(0);
    s_bottom = s_top - size;
}

unsigned cstack_limit(unsigned ccalls)
{
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    if (!s_top || sp > s_top || sp < s_bottom) return CSTACK_MAX_CCALLS;   /* not the main task */
    if (sp >= s_bottom + CSTACK_SOFT) {
        s_err_at = 0;
        return CSTACK_MAX_CCALLS;
    }
    if (sp < s_bottom + CSTACK_HARD) return 0;          /* Lua: LUA_ERRERR */
    if (!s_err_at || ccalls <= s_err_at) {
        s_err_at = ccalls;
        return ccalls;                                  /* Lua: "C stack overflow" */
    }
    return CSTACK_MAX_CCALLS;                           /* the error's handler */
}
