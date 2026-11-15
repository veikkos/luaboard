/* <sys/time.h> for MSVC. struct timeval is Winsock's (posix_compat.h). */
#pragma once
#include "../posix_compat.h"

int gettimeofday(struct timeval *tv, void *tz);
int settimeofday(const struct timeval *tv, const void *tz);
