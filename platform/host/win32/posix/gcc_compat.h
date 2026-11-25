/*
 * GCC attributes and builtins the sources use, for MSVC. A part of
 * posix_compat.h, and the only part the unit tests of the pure logic need
 * (fw-unit: no POSIX there), forced into their sources too (/FI).
 */
#pragma once
#ifdef _MSC_VER
#ifndef __attribute__
#define __attribute__(x)
#endif
#include <intrin.h>
#define __builtin_frame_address(level) ((void *)_AddressOfReturnAddress())
#endif
