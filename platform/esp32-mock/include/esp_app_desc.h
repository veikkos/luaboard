/* ESP32 mock platform - subset of ESP-IDF <esp_app_desc.h>. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t magic_word;
    uint32_t secure_version;
    uint32_t reserv1[2];
    char version[32];        /* the build's `git describe` */
    char project_name[32];
    char time[16];
    char date[16];
    char idf_ver[32];
    uint8_t app_elf_sha256[32];
} esp_app_desc_t;

/** The running app's description. */
const esp_app_desc_t *esp_app_get_description(void);

#ifdef __cplusplus
}
#endif
