#ifndef IMAGE_CRYPTO_H
#define IMAGE_CRYPTO_H

#include <stdint.h>
#include "image_verify.h"

/*
 * Trust model: the signature covers the first IMAGE_SIGNED_LEN bytes of the header
 * (magic, version, image_size, image_crc32, sha256), so version and size cannot be altered
 * without breaking it. The sha256 field binds the payload. Signature = ECDSA P-256 over
 * SHA-256(signed header prefix), raw r||s.
 */
#define IMAGE_SIGNED_LEN 48u   /* offsetof(image_header_t, signature) */

/* Public key supplied by the build (build/pubkey.h) or by the host test as an override. */
extern const uint8_t safeflash_pubkey[64];

/* Checks payload SHA-256 against the header, then the ECDSA signature. Header must already be structurally valid. */
img_status_t image_check_signature(uint32_t slot_addr, const image_header_t *h);

#endif
