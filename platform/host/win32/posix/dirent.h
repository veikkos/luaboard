/* <dirent.h> for MSVC: opendir / readdir / closedir over FindFirstFile (posix_win32.c). */
#pragma once
#include "posix_compat.h"

#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8

struct dirent {
    unsigned char d_type;
    char d_name[MAX_PATH];
};

typedef struct posix_dir DIR;

DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);
