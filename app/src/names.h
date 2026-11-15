/* The names the device accepts for apps and their files: 1 to max-1
   characters from A-Z a-z 0-9 _ . -, not starting with '.'. Pure logic, for
   vfs.c and the marketplace index (apps_index.c). */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/** Whether s[0..n) is such a name, shorter than max. */
bool name_valid(const char *s, size_t n, size_t max);
