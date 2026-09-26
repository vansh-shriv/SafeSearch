#include "image_verify.h"
#include "crc32.h"
#include "flash_hal.h"
#include "flash_map.h"

#include <stddef.h>

img_status_t image_check_basic(uint32_t slot_addr, uint32_t min_allowed_version,
                               image_header_t *hdr_out)
{
    image_header_t h;

    if (flash_read(slot_addr, &h, sizeof h) != 0)
        return IMG_ERR_READ;
    if (h.magic != IMAGE_MAGIC)
        return IMG_ERR_MAGIC;
    if (h.header_crc32 != crc32_calc(&h, offsetof(image_header_t, header_crc32)))
        return IMG_ERR_HEADER_CRC;
    if (h.image_size == 0 || h.image_size > IMAGE_MAX_SIZE)
        return IMG_ERR_SIZE;
    if (h.version < min_allowed_version)
        return IMG_ERR_VERSION_FLOOR;

    /* Stream the payload in chunks; the bootloader has little RAM to spare. */
    uint8_t buf[256];
    uint32_t crc = 0;
    uint32_t off = 0;
    while (off < h.image_size) {
        uint32_t n = h.image_size - off;
        if (n > sizeof buf)
            n = sizeof buf;
        if (flash_read(slot_addr + IMAGE_HEADER_SIZE + off, buf, n) != 0)
            return IMG_ERR_READ;
        crc = crc32_update(crc, buf, n);
        off += n;
    }
    if (crc != h.image_crc32)
        return IMG_ERR_IMAGE_CRC;

    if (hdr_out)
        *hdr_out = h;
    return IMG_OK;
}
