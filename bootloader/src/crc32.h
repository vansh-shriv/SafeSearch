#ifndef CRC32_H
#define CRC32_H

#include <stdint.h>
#include <stddef.h>

/* IEEE 802.3 CRC-32 (reflected, poly 0xEDB88320), same as zlib/Python binascii.crc32. */
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);

static inline uint32_t crc32_calc(const void *data, size_t len)
{
    return crc32_update(0, data, len);
}

#endif
