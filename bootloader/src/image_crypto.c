#include "image_crypto.h"
#include "flash_hal.h"
#include "flash_map.h"
#include "sha256.h"
#include "uECC.h"

#include <stddef.h>

_Static_assert(offsetof(image_header_t, signature) == IMAGE_SIGNED_LEN, "signed prefix length mismatch");

img_status_t image_check_signature(uint32_t slot_base, const image_header_t *h)
{
    sha256_ctx ctx;
    uint8_t buf[256];
    uint8_t digest[32];

    /* 1. Payload hash must match the (signed) hash in the header. */
    sha256_init(&ctx);
    for (uint32_t off = 0; off < h->image_size;) {
        uint32_t n = h->image_size - off;
        if (n > sizeof buf)
            n = sizeof buf;
        if (flash_read(slot_base + IMAGE_HEADER_SIZE + off, buf, n) != 0)
            return IMG_ERR_READ;
        sha256_update(&ctx, buf, n);
        off += n;
    }
    sha256_final(&ctx, digest);

    uint8_t diff = 0;
    for (int i = 0; i < 32; i++)
        diff |= digest[i] ^ h->sha256[i];
    if (diff)
        return IMG_ERR_HASH;

    /* 2. Signature over the header prefix. */
    sha256_init(&ctx);
    sha256_update(&ctx, h, IMAGE_SIGNED_LEN);
    sha256_final(&ctx, digest);

    if (!uECC_verify(safeflash_pubkey, digest, sizeof digest, h->signature, uECC_secp256r1()))
        return IMG_ERR_SIGNATURE;
    return IMG_OK;
}
