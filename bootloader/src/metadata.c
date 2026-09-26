#include "metadata.h"
#include "crc32.h"
#include "flash_hal.h"
#include "flash_map.h"

#include <stddef.h>

static const uint32_t meta_addr[2] = { META_ADDR_A, META_ADDR_B };
static const uint32_t meta_sector[2] = { META_SECTOR_A, META_SECTOR_B };

static int copy_valid(const boot_metadata_t *m)
{
    return m->magic == META_MAGIC &&
           m->crc32 == crc32_calc(m, offsetof(boot_metadata_t, crc32)) &&
           m->active_slot < SLOT_COUNT &&
           m->boot_state <= BOOT_STATE_REVERT_PENDING;
}

/* Wrap-safe: true if a is newer than b. */
static int seq_newer(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

/* Returns index (0/1) of newest valid copy, or -1. */
static int find_newest(boot_metadata_t *out)
{
    boot_metadata_t m[2];
    int ok[2];

    for (int i = 0; i < 2; i++)
        ok[i] = flash_read(meta_addr[i], &m[i], sizeof m[i]) == 0 && copy_valid(&m[i]);

    int best = -1;
    for (int i = 0; i < 2; i++) {
        if (ok[i] && (best < 0 || seq_newer(m[i].seq, m[best].seq)))
            best = i;
    }
    if (best >= 0 && out)
        *out = m[best];
    return best;
}

int metadata_load(boot_metadata_t *out)
{
    return find_newest(out) < 0 ? -1 : 0;
}

int metadata_store(boot_metadata_t *md)
{
    boot_metadata_t cur;
    int newest = find_newest(&cur);
    int target = newest < 0 ? 0 : 1 - newest;

    md->magic = META_MAGIC;
    md->seq = newest < 0 ? 1 : cur.seq + 1;
    md->crc32 = crc32_calc(md, offsetof(boot_metadata_t, crc32));

    if (flash_erase_sector(meta_sector[target]) != 0)
        return -1;
    return flash_write(meta_addr[target], md, sizeof *md);
}
