#ifndef METADATA_H
#define METADATA_H

#include <stdint.h>

#define META_MAGIC 0x4D455441u /* "META" */

typedef enum {
    BOOT_STATE_NORMAL = 0,
    BOOT_STATE_TRIAL = 1,
    BOOT_STATE_CONFIRMED = 2,
    BOOT_STATE_REVERT_PENDING = 3,
} boot_state_t;

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint8_t  active_slot;
    uint8_t  boot_state;
    uint8_t  trial_count;
    uint8_t  reserved;
    uint32_t min_allowed_version;
    uint32_t crc32;            /* over every field above */
} boot_metadata_t;

/*
 * Load the newest valid copy. Returns 0 and fills *out, or -1 if neither copy
 * is valid (first boot / corruption).
 */
int metadata_load(boot_metadata_t *out);

/*
 * Commit *md as the new state. Assigns seq = newest_valid_seq + 1, computes the
 * CRC, and writes into the copy that is NOT the current newest, so a power cut
 * at any point leaves the previous copy intact.
 */
int metadata_store(boot_metadata_t *md);

#endif
