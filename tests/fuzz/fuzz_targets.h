#ifndef FUZZ_TARGETS_H
#define FUZZ_TARGETS_H

#include <stddef.h>
#include <stdint.h>

/*
 * Property-checking fuzz targets over the production boot / update / metadata code. Each returns 0 and
 * aborts (via FUZZ_ASSERT) on a violated property, so any fuzzer treats a violation as a crash.
 *
 * They are driven two ways: libFuzzer (fuzz_libfuzzer.c, clang + ASan/UBSan in CI) and a portable
 * mutation driver (fuzz_driver.c) that runs anywhere, including MinGW without sanitizers.
 */
int fuzz_image(const uint8_t *data, size_t size);      /* arbitrary slot contents vs the verifier */
int fuzz_metadata(const uint8_t *data, size_t size);   /* arbitrary metadata sectors */
int fuzz_boot(const uint8_t *data, size_t size);       /* whole-device state -> boot_decide */
int fuzz_install(const uint8_t *data, size_t size);    /* arbitrary blob -> sf_install_update */
int fuzz_recovery(const uint8_t *data, size_t size);   /* arbitrary byte stream -> serial recovery */

/* One-time setup (loads reference images from build/). Returns nonzero on failure. */
int fuzz_init(void);

/* Seeds: genuine images the mutators start from. */
size_t fuzz_seed_count(void);
const uint8_t *fuzz_seed(size_t i, size_t *len);

/* Seeds for fuzz_recovery: [flags][valid full-transfer stream]. */
size_t fuzz_recovery_seed_count(void);
const uint8_t *fuzz_recovery_seed(size_t i, size_t *len);

/* Coarse "feature" the last run reached (outcome codes), used by the driver as cheap feedback. */
uint32_t fuzz_last_feature(void);

#endif
