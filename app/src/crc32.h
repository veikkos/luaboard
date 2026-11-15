/* CRC-32 (IEEE 802.3, same as zlib/PNG). Used to verify script uploads. */
#pragma once

#include <stddef.h>
#include <stdint.h>

uint32_t crc32_update(uint32_t crc, const void *data, size_t len);

static inline uint32_t crc32_calc(const void *data, size_t len)
{
    return crc32_update(0, data, len);
}
