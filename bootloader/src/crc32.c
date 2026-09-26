#include "crc32.h"

/* Bitwise implementation: no 1 KB table, keeps the bootloader small. */
uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = data;

    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
