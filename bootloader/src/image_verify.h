#ifndef IMAGE_VERIFY_H
#define IMAGE_VERIFY_H

#include <stdint.h>

#define IMAGE_MAGIC 0x53414645u /* "SAFE" */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t image_size;       /* payload bytes following the header region */
    uint32_t image_crc32;
    uint8_t  sha256[32];
    uint8_t  signature[64];
    uint32_t header_crc32;     /* over every field above */
} image_header_t;

typedef enum {
    IMG_OK = 0,
    IMG_ERR_READ,
    IMG_ERR_MAGIC,
    IMG_ERR_HEADER_CRC,
    IMG_ERR_SIZE,
    IMG_ERR_VERSION_FLOOR,
    IMG_ERR_IMAGE_CRC,
    IMG_ERR_HASH,
    IMG_ERR_SIGNATURE,
} img_status_t;

/*
 * Structural validation only (magic, header CRC, size bounds, version floor,
 * payload CRC). Cryptographic verification is a separate step (Phase 3).
 */
img_status_t image_check_basic(uint32_t slot_addr, uint32_t min_allowed_version,
                               image_header_t *hdr_out);

#endif
