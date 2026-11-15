/* <unistd.h> for MSVC: the part the firmware and the mock use (posix_compat.h).
   close, read, write, unlink, rmdir, access, getpid, fileno and isatty are
   the CRT's own POSIX names (io.h, direct.h, process.h). */
#pragma once
#include "posix_compat.h"

#include <process.h>

#define F_OK 0
#define W_OK 2
#define R_OK 4
/* MSVC's access() rejects X_OK; anything that exists counts (there is no
   executable bit). */
#define X_OK 0

int usleep(useconds_t us);
unsigned sleep(unsigned seconds);
