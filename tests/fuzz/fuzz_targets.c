#include "fuzz_targets.h"

#include "boot_config.h"
#include "boot_logic.h"
#include "crc32.h"
#include "flash_hal.h"
#include "flash_map.h"
#include "image_crypto.h"
#include "image_verify.h"
#include "metadata.h"
#include "mock_flash.h"
#include "recovery.h"
#include "recovery_env.h"
#include "safeflash_app.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_ASSERT(c)                                                                    \
    do {                                                                                  \
        if (!(c)) {                                                                       \
            fprintf(stderr, "FUZZ PROPERTY VIOLATED: %s (%s:%d)\n", #c, __FILE__, __LINE__); \
            abort();                                                                      \
        }                                                                                 \
    } while (0)

/* boot_logic.c console hooks: silent */
void boot_puts(const char *s) { (void)s; }
void boot_puthex(uint32_t v) { (void)v; }

static uint8_t ref[2][SLOT_SIZE];     /* genuine signed images: [0] v1 (slot A), [1] v2 (slot B) */
static size_t ref_len[2];
static uint32_t ref_version[2] = { 1, 2 };
static uint8_t factory[MOCK_FLASH_SIZE];   /* v1 in slot A, metadata initialised, floor 1 */
static uint32_t feature;
static int inited;

uint32_t fuzz_last_feature(void) { return feature; }

static size_t load_file(const char *path, uint8_t *dst, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f)
        return 0;
    n = fread(dst, 1, cap, f);
    fclose(f);
    return n;
}

int fuzz_init(void)
{
    boot_decision_t d;

    if (inited)
        return 0;
    ref_len[0] = load_file("../../build/app_v1_slotA.img", ref[0], sizeof ref[0]);
    ref_len[1] = load_file("../../build/app_v2_slotB.img", ref[1], sizeof ref[1]);
    if (!ref_len[0] || !ref_len[1]) {
        fprintf(stderr, "cannot load build/app_v*.img (run `mingw32-make` first)\n");
        return 1;
    }
    mock_flash_reset();
    flash_write(SLOT_A_ADDR + IMAGE_HEADER_SIZE, ref[0] + IMAGE_HEADER_SIZE, ref_len[0] - IMAGE_HEADER_SIZE);
    flash_write(SLOT_A_ADDR, ref[0], IMAGE_HEADER_SIZE);
    memset(&d, 0, sizeof d);
    if (boot_decide(&d) != 0 || sf_confirm_healthy() != 0)
        return 1;
    mock_flash_save(factory);
    inited = 1;
    return 0;
}

size_t fuzz_seed_count(void) { return 2; }
const uint8_t *fuzz_seed(size_t i, size_t *len) { *len = ref_len[i]; return ref[i]; }

/* ---- helpers ---- */

static void put_u32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get_u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/*
 * Structure-aware fixups so the fuzzer gets past the unkeyed CRCs and actually reaches hash and signature
 * checks. flags: 1 = header CRC, 2 = payload CRC field, 4 = payload SHA-256 field.
 */
static void fixup_image(uint8_t *img, size_t len, unsigned flags)
{
    uint32_t size;

    if (len < sizeof(image_header_t))
        return;
    size = get_u32(img + 8);
    if ((flags & 6) && size > 0 && size <= IMAGE_MAX_SIZE && IMAGE_HEADER_SIZE + (size_t)size <= len) {
        if (flags & 2)
            put_u32(img + 12, crc32_calc(img + IMAGE_HEADER_SIZE, size));
        if (flags & 4) {
            sha256_ctx c;
            sha256_init(&c);
            sha256_update(&c, img + IMAGE_HEADER_SIZE, size);
            sha256_final(&c, img + 16);
        }
    }
    if (flags & 7)
        put_u32(img + offsetof(image_header_t, header_crc32),
                crc32_calc(img, offsetof(image_header_t, header_crc32)));
}

/* Does the slot in flash hold a byte-exact genuine image (everything the verifier covers)? */
static int slot_is_genuine(uint8_t slot, uint32_t *version)
{
    const uint8_t *s = mock_flash_raw(slot_addr(slot));

    for (int i = 0; i < 2; i++) {
        size_t payload = ref_len[i] - IMAGE_HEADER_SIZE;
        if (memcmp(s, ref[i], 116) == 0 &&
            memcmp(s + IMAGE_HEADER_SIZE, ref[i] + IMAGE_HEADER_SIZE, payload) == 0) {
            if (version)
                *version = ref_version[i];
            return 1;
        }
    }
    return 0;
}

/* ---- target 1: arbitrary slot contents vs the verifier ---- */
int fuzz_image(const uint8_t *data, size_t size)
{
    static uint8_t buf[SLOT_SIZE];
    image_header_t h;
    unsigned flags;
    uint8_t slot;
    img_status_t s;

    if (size < 2 || fuzz_init())
        return 0;
    flags = data[0];
    slot = (data[0] >> 3) & 1;
    size--;
    if (size > sizeof buf)
        size = sizeof buf;
    memcpy(buf, data + 1, size);
    fixup_image(buf, size, flags & 7);

    mock_flash_reset();
    memcpy(mock_flash_raw(slot_addr(slot)), buf, size);

    s = image_check_basic(slot_addr(slot), 0, &h);
    feature = (uint32_t)s;
    if (s == IMG_OK) {
        FUZZ_ASSERT(h.image_size > 0 && h.image_size <= IMAGE_MAX_SIZE);
        s = image_check_signature(slot_addr(slot), &h);
        feature = 16 + (uint32_t)s;
        if (s == IMG_OK) {
            /* Only a byte-exact genuine image may ever verify; anything else means the checks are broken. */
            FUZZ_ASSERT(slot_is_genuine(slot, NULL));
        }
    }
    return 0;
}

/* ---- target 2: arbitrary metadata sectors ---- */
int fuzz_metadata(const uint8_t *data, size_t size)
{
    boot_metadata_t out, in;
    unsigned flags;

    if (size < 1 + 2 * sizeof(boot_metadata_t) || fuzz_init())
        return 0;
    flags = data[0];
    mock_flash_reset();
    for (int c = 0; c < 2; c++) {
        boot_metadata_t m;
        memcpy(&m, data + 1 + c * sizeof m, sizeof m);
        if (flags & (1u << c)) {   /* fix CRC (and magic) so the copy looks valid */
            m.magic = META_MAGIC;
            m.crc32 = crc32_calc(&m, offsetof(boot_metadata_t, crc32));
        }
        memcpy(mock_flash_raw(c == 0 ? META_ADDR_A : META_ADDR_B), &m, sizeof m);
    }

    if (metadata_load(&out) == 0) {
        FUZZ_ASSERT(out.magic == META_MAGIC);
        FUZZ_ASSERT(out.active_slot < SLOT_COUNT);
        FUZZ_ASSERT(out.boot_state <= BOOT_STATE_REVERT_PENDING);
        FUZZ_ASSERT(out.crc32 == crc32_calc(&out, offsetof(boot_metadata_t, crc32)));
        feature = 1;

        /* Store must commit exactly what was asked, with a seq newer than what it replaced. */
        memcpy(&in, &out, sizeof in);
        in.active_slot = (uint8_t)(1 - out.active_slot);
        in.trial_count = (uint8_t)(out.trial_count + 1);
        FUZZ_ASSERT(metadata_store(&in) == 0);
        {
            boot_metadata_t back;
            FUZZ_ASSERT(metadata_load(&back) == 0);
            FUZZ_ASSERT(back.seq == out.seq + 1);
            FUZZ_ASSERT(back.active_slot == in.active_slot);
            FUZZ_ASSERT(back.trial_count == in.trial_count);
            FUZZ_ASSERT(back.min_allowed_version == out.min_allowed_version);
        }
    } else {
        feature = 0;
    }
    return 0;
}

/* ---- target 3: whole-device state -> boot_decide (crypto is real) ---- */
int fuzz_boot(const uint8_t *data, size_t size)
{
    boot_metadata_t md, pre, post;
    boot_decision_t d;
    int have_pre;
    unsigned mode;

    if (size < 1 + sizeof(boot_metadata_t) + 4 || fuzz_init())
        return 0;
    mode = data[0];
    mock_flash_reset();

    /*
     * Slot contents, per slot: 0 blank, 1 genuine, 2 corrupted, 3 forged-signature. Corruptions are built so
     * they get past the unkeyed CRCs and reach the hash / signature checks: kind 2 flips a payload byte (and
     * fixes both CRCs, leaving the signed hash stale, or leaves the CRC broken when input bit 0 is clear);
     * kind 3 flips a signature byte and fixes the header CRC.
     */
    for (int slot = 0; slot < 2; slot++) {
        unsigned kind = (mode >> (slot * 2)) & 3;
        int which = slot;   /* genuine v1 in A, v2 in B; swapped when bit 4 is set */
        uint8_t *s = mock_flash_raw(slot_addr(slot));
        if (mode & 16)
            which = 1 - slot;
        if (kind == 0)
            continue;   /* blank */
        memcpy(s, ref[which], ref_len[which]);
        if (kind == 2) {
            size_t off = IMAGE_HEADER_SIZE + (get_u32(data + 1 + sizeof(boot_metadata_t)) + (size_t)slot * 977) %
                                                 (ref_len[which] - IMAGE_HEADER_SIZE);
            s[off] ^= (uint8_t)(1u << (data[1] & 7));
            if (data[2] & 1)
                fixup_image(s, ref_len[which], 3);
        } else if (kind == 3) {
            s[48 + (data[1] % 64)] ^= (uint8_t)(1u << (data[2] & 7));
            fixup_image(s, ref_len[which], 1);
        }
    }
    /*
     * Metadata: written with a valid CRC so the fuzzer explores states, not CRC luck. Most of the time the
     * fields are folded into the small ranges that matter (real versions are 1 and 2, trial counts near
     * MAX_TRIALS); input bit 64 keeps the raw full-range values.
     */
    memcpy(&md, data + 1, sizeof md);
    if (mode & 32) {
        md.magic = META_MAGIC;
        md.active_slot &= 1;
        md.boot_state &= 3;
        if (!(mode & 64)) {
            md.trial_count %= (MAX_TRIALS + 2);
            md.min_allowed_version %= 4;
        }
        md.crc32 = crc32_calc(&md, offsetof(boot_metadata_t, crc32));
        memcpy(mock_flash_raw(META_ADDR_A), &md, sizeof md);
    }

    have_pre = metadata_load(&pre) == 0;
    memset(&d, 0, sizeof d);
    if (boot_decide(&d) != 0) {
        feature = 100;
        return 0;   /* nothing bootable: acceptable, must not crash */
    }
    /* feedback: which decision path was taken (slot, trial, and whether it came from a revert/fallback) */
    feature = 1 + d.slot + 2 * d.trial + 4 * (have_pre ? 1 + pre.boot_state : 0) +
              32 * (have_pre && pre.active_slot != d.slot);

    /* P1: we only ever boot a byte-exact genuine image, from a slot with a valid signature */
    {
        uint32_t v = 0;
        FUZZ_ASSERT(slot_is_genuine(d.slot, &v));
        FUZZ_ASSERT(v == d.version);
    }
    /* P2: metadata is valid afterwards and consistent with the decision */
    FUZZ_ASSERT(metadata_load(&post) == 0);
    FUZZ_ASSERT(post.active_slot == d.slot);
    /* P3: floor never decreases, and the booted version respects it */
    if (have_pre)
        FUZZ_ASSERT(post.min_allowed_version >= pre.min_allowed_version);
    FUZZ_ASSERT(d.version >= post.min_allowed_version);
    /* P4: trial bookkeeping */
    if (post.boot_state == BOOT_STATE_TRIAL)   /* the counter is meaningless outside a trial */
        FUZZ_ASSERT(post.trial_count <= MAX_TRIALS);
    if (d.trial) {
        FUZZ_ASSERT(post.boot_state == BOOT_STATE_TRIAL && post.trial_count == d.trial_no);
        FUZZ_ASSERT(d.trial_no >= 1 && d.trial_no <= MAX_TRIALS);
    }
    /* P5: an app that never confirms must converge (revert, or run out of bootable slots), never loop */
    for (unsigned i = 0; i < MAX_TRIALS + 3 && d.trial; i++) {
        memset(&d, 0, sizeof d);
        if (boot_decide(&d) != 0) {
            feature = 101;
            return 0;
        }
        FUZZ_ASSERT(slot_is_genuine(d.slot, NULL));
    }
    FUZZ_ASSERT(!d.trial);
    return 0;
}

/* ---- target 4: arbitrary blob -> sf_install_update ---- */
int fuzz_install(const uint8_t *data, size_t size)
{
    static uint8_t before[MOCK_FLASH_SIZE];
    static uint8_t blob[SLOT_SIZE + 64];
    boot_metadata_t pre, post;
    unsigned flags;
    int rc, nops;

    if (size < 2 || fuzz_init())
        return 0;
    flags = data[0];
    size--;
    if (size > sizeof blob)
        size = sizeof blob;
    memcpy(blob, data + 1, size);
    fixup_image(blob, size, flags & 7);

    mock_flash_restore(factory);
    {
        /* vary the anti-rollback floor (0..3) so both accepted and refused versions are reachable */
        boot_metadata_t m;
        FUZZ_ASSERT(metadata_load(&m) == 0);
        m.min_allowed_version = (flags >> 4) & 3;
        FUZZ_ASSERT(metadata_store(&m) == 0);
    }
    mock_flash_save(before);
    mock_flash_restore(before);   /* clears the mutation trace so the checks below see only the install */
    FUZZ_ASSERT(metadata_load(&pre) == 0);

    rc = sf_install_update(blob, size);
    feature = (uint32_t)(-rc);
    nops = mock_flash_mutation_count();

    /* P1: the running slot (A) is never touched, byte for byte */
    FUZZ_ASSERT(memcmp(before + (SLOT_A_ADDR - FLASH_BASE), mock_flash_raw(SLOT_A_ADDR), SLOT_SIZE) == 0);
    /* P2: every flash mutation lands in slot B's sectors or the metadata sectors, nowhere else */
    for (int i = 0; i < nops && i < MOCK_MAX_OPS; i++) {
        uint32_t a = mock_flash_op_addr(i);
        size_t l = mock_flash_op_len(i);
        if (mock_flash_op_kind(i) == MOCK_OP_ERASE) {
            FUZZ_ASSERT((a >= SLOT_B_FIRST_SECTOR && a < SLOT_B_FIRST_SECTOR + SLOT_SECTOR_COUNT) ||
                        a == META_SECTOR_A || a == META_SECTOR_B);
        } else {
            int in_b = a >= SLOT_B_ADDR && a + l <= SLOT_B_ADDR + SLOT_SIZE;
            int in_meta = (a >= META_ADDR_A && a + l <= META_ADDR_A + META_SECTOR_SIZE) ||
                          (a >= META_ADDR_B && a + l <= META_ADDR_B + META_SECTOR_SIZE);
            FUZZ_ASSERT(in_b || in_meta);
        }
    }
    /* P3: metadata is always valid afterwards; on success it points at slot B in TRIAL; on refusal it is unchanged */
    FUZZ_ASSERT(metadata_load(&post) == 0);
    if (rc == SF_OK) {
        FUZZ_ASSERT(post.active_slot == 1 && post.boot_state == BOOT_STATE_TRIAL && post.trial_count == 0);
        FUZZ_ASSERT(post.min_allowed_version == pre.min_allowed_version);
    } else {
        FUZZ_ASSERT(post.seq == pre.seq && post.active_slot == pre.active_slot);
        FUZZ_ASSERT(nops == 0);   /* refused images cause no flash activity at all */
    }
    /* P4: the anti-rollback floor is enforced by the installer */
    if (rc == SF_OK)
        FUZZ_ASSERT(get_u32(blob + 4) >= pre.min_allowed_version);
    return 0;
}

/* ---- target 5: arbitrary byte stream -> serial recovery ---- */

/* Recompute the CRC of every well-formed frame in place, so mutations reach the protocol logic. */
static void fixup_stream(uint8_t *s, size_t len)
{
    size_t i = 0;

    while (i + 8 <= len) {
        uint16_t l;
        uint32_t crc;
        if (s[i] != REC_SOF) {
            i++;
            continue;
        }
        l = (uint16_t)(s[i + 2] | (s[i + 3] << 8));
        if (l > REC_MAX_PAYLOAD || i + 8u + l > len) {
            i++;
            continue;
        }
        crc = crc32_calc(s + i + 1, 3u + l);
        put_u32(s + i + 4 + l, crc);
        i += 8u + l;
    }
}

static uint8_t rec_seed_buf[2][SLOT_SIZE * 2];
static size_t rec_seed_len[2];

size_t fuzz_recovery_seed_count(void) { return 2; }

const uint8_t *fuzz_recovery_seed(size_t i, size_t *len)
{
    if (!rec_seed_len[i]) {
        rec_seed_buf[i][0] = (uint8_t)i;   /* flags: bit0 = device has a healthy image in slot A */
        rec_seed_len[i] = 1 + rec_stream_from_image(rec_seed_buf[i] + 1, ref[1], ref_len[1]);
    }
    *len = rec_seed_len[i];
    return rec_seed_buf[i];
}

int fuzz_recovery(const uint8_t *data, size_t size)
{
    static uint8_t before[MOCK_FLASH_SIZE];
    static uint8_t stream[SLOT_SIZE * 2];
    boot_metadata_t pre, post;
    unsigned flags;
    uint8_t target;
    int rc, nops, touched_meta = 0, healthy;
    uint32_t tfirst;

    if (size < 2 || fuzz_init())
        return 0;
    flags = data[0];
    healthy = flags & 1;   /* 1: v1 still runs from slot A; 0: slot A wiped, nothing bootable */
    size--;
    if (size > sizeof stream)
        size = sizeof stream;
    memcpy(stream, data + 1, size);
    if (flags & 2)
        fixup_stream(stream, size);

    mock_flash_restore(factory);
    if (!healthy)
        (void)flash_erase_sector(SLOT_A_FIRST_SECTOR);
    mock_flash_save(before);
    mock_flash_restore(before);   /* clears the mutation trace */
    FUZZ_ASSERT(metadata_load(&pre) == 0);
    target = (uint8_t)(1 - pre.active_slot);
    tfirst = target == 0 ? SLOT_A_FIRST_SECTOR : SLOT_B_FIRST_SECTOR;

    rec_env_set_input(stream, size);
    rc = recovery_run();
    nops = mock_flash_mutation_count();
    feature = (uint32_t)(rc + 2) + 4u * (uint32_t)(nops > 12 ? 12 : nops);

    /* P1: only the target slot's sectors and (on success) the metadata sectors are ever touched */
    for (int i = 0; i < nops && i < MOCK_MAX_OPS; i++) {
        uint32_t a = mock_flash_op_addr(i);
        size_t l = mock_flash_op_len(i);
        if (mock_flash_op_kind(i) == MOCK_OP_ERASE) {
            if (a == META_SECTOR_A || a == META_SECTOR_B)
                touched_meta = 1;
            else
                FUZZ_ASSERT(a >= tfirst && a < tfirst + SLOT_SECTOR_COUNT);
        } else {
            uint32_t base = slot_addr(target);
            int in_target = a >= base && a + l <= base + SLOT_SIZE;
            int in_meta = (a >= META_ADDR_A && a + l <= META_ADDR_A + META_SECTOR_SIZE) ||
                          (a >= META_ADDR_B && a + l <= META_ADDR_B + META_SECTOR_SIZE);
            if (in_meta)
                touched_meta = 1;
            FUZZ_ASSERT(in_target || in_meta);
        }
    }
    /* P2: a healthy running image in the other slot is never disturbed */
    if (healthy)
        FUZZ_ASSERT(memcmp(before + (slot_addr(1 - target) - FLASH_BASE), mock_flash_raw(slot_addr(1 - target)), SLOT_SIZE) == 0);
    /* P3: metadata is always valid afterwards */
    FUZZ_ASSERT(metadata_load(&post) == 0);
    /* P4: metadata changes only when an image was fully received, verified, and installed */
    if (rc == RECOVERY_INSTALLED) {
        FUZZ_ASSERT(post.active_slot == target && post.boot_state == BOOT_STATE_TRIAL && post.trial_count == 0);
        FUZZ_ASSERT(post.min_allowed_version == pre.min_allowed_version);
        FUZZ_ASSERT(slot_is_genuine(target, NULL));
        {
            boot_decision_t d;
            memset(&d, 0, sizeof d);
            FUZZ_ASSERT(boot_decide(&d) == 0 && d.slot == target);
        }
    } else {
        FUZZ_ASSERT(!touched_meta);
        FUZZ_ASSERT(post.seq == pre.seq && post.active_slot == pre.active_slot);
    }
    return 0;
}
