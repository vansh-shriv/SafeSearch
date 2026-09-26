/*
 * Explicit-state model checker over the PRODUCTION boot / metadata / confirm / install code.
 *
 * What is real: boot_logic.c (boot_decide), metadata.c, safeflash_app.c (sf_confirm_healthy),
 * safeflash_update.c (sf_install_update), crc32.c. What is abstract: the cryptography and the slot payloads.
 * A slot's content is one of  INVALID (blank/torn) | V1 | V2 | V3 (validly signed images of that version) |
 * FORGED (passes the unkeyed CRCs, fails the signature). image_check_basic / image_check_signature are
 * replaced by an oracle over that content; the real crypto is covered by the host sweep and the fuzzers.
 *
 * Exploration: breadth-first over all reachable device states (metadata copy A, copy B, slot A, slot B).
 * From each state, every environment action is applied:
 *     BOOT then hang | BOOT then confirm | BOOT then install {V1, V2, V3, FORGED}
 * each with no power cut and with a power cut at EVERY flash mutation the action performs, torn writes at
 * several byte counts and three torn-erase patterns. In addition an adversary may TAMPER: overwrite either
 * slot with any content at rest (including old validly signed images, i.e. a downgrade attack, forged images,
 * or plain corruption) without touching the metadata. Properties are checked on every transition.
 *
 * Properties (violations abort with a counter-example trace):
 *   S1  the decided slot always holds a VALID image whose version is >= the anti-rollback floor
 *   S2  the floor never decreases across any transition, power cuts included
 *   S3  if a metadata copy was valid before a transition, one is valid after it
 *   S4  the installer never modifies the slot it runs from
 *   S5  once confirm succeeds the ratchet has really risen: floor >= confirmed version, and state is not TRIAL
 *   L1  boot fails only when nothing legitimately bootable exists (or a trial is exhausted with no fallback)
 *   L2  an app that never confirms cannot loop forever: repeated boots reach a stable state or run out of images
 */
#include "boot_config.h"
#include "boot_logic.h"
#include "crc32.h"
#include "flash_hal.h"
#include "flash_map.h"
#include "image_crypto.h"
#include "image_verify.h"
#include "metadata.h"
#include "safeflash_app.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ abstract world / HAL */
enum { C_INVALID = 0, C_V1 = 1, C_V2 = 2, C_V3 = 3, C_FORGED = 4, C_KINDS = 5 };
#define FORGED_VERSION 9   /* a forged image claims a very high version */

static uint8_t meta_mem[2][META_SECTOR_SIZE];
static int slot_content[2];

static int op_count, cut_at, cut_partial, dead;
static int mutations_seen;

void boot_puts(const char *s) { (void)s; }
void boot_puthex(uint32_t v) { (void)v; }

static int content_version(int c) { return c == C_FORGED ? FORGED_VERSION : c; }

static int slot_of_addr(uint32_t a, int *first_byte)
{
    if (a >= SLOT_A_ADDR && a < SLOT_A_ADDR + SLOT_SIZE) { *first_byte = a == SLOT_A_ADDR; return 0; }
    if (a >= SLOT_B_ADDR && a < SLOT_B_ADDR + SLOT_SIZE) { *first_byte = a == SLOT_B_ADDR; return 1; }
    return -1;
}

int flash_read(uint32_t addr, void *buf, size_t len)
{
    int fb, s;

    if (addr >= META_ADDR_A && addr + len <= META_ADDR_A + META_SECTOR_SIZE) { memcpy(buf, &meta_mem[0][addr - META_ADDR_A], len); return 0; }
    if (addr >= META_ADDR_B && addr + len <= META_ADDR_B + META_SECTOR_SIZE) { memcpy(buf, &meta_mem[1][addr - META_ADDR_B], len); return 0; }
    s = slot_of_addr(addr, &fb);
    if (s >= 0 && fb && len == sizeof(image_header_t)) {   /* the app reads its own header to learn its version */
        image_header_t h;
        memset(&h, 0xFF, sizeof h);
        if (slot_content[s] != C_INVALID) {
            h.magic = IMAGE_MAGIC;
            h.version = (uint32_t)content_version(slot_content[s]);
        }
        memcpy(buf, &h, len);
        return 0;
    }
    return -1;
}

int flash_erase_sector(uint32_t sector)
{
    int idx = op_count++;

    mutations_seen++;
    if (dead)
        return -1;
    if (sector == META_SECTOR_A || sector == META_SECTOR_B) {
        uint8_t *m = meta_mem[sector == META_SECTOR_B];
        if (idx == cut_at) {
            switch (cut_partial % 3) {
            case 0: break;                                              /* erase never took effect */
            case 1: memset(m, 0xA5, META_SECTOR_SIZE); break;          /* garbage */
            case 2: memset(m, 0xFF, META_SECTOR_SIZE / 2); break;      /* half erased */
            }
            dead = 1;
            return -1;
        }
        memset(m, 0xFF, META_SECTOR_SIZE);
        return 0;
    }
    /* a slot sector: the slot is no longer a valid image, whether the erase completed or was torn */
    {
        int s = sector >= SLOT_B_FIRST_SECTOR ? 1 : 0;
        slot_content[s] = C_INVALID;
    }
    if (idx == cut_at) {
        dead = 1;
        return -1;
    }
    return 0;
}

int flash_write(uint32_t addr, const void *buf, size_t len)
{
    const uint8_t *src = buf;
    int idx = op_count++, cut = idx == cut_at, fb, s;
    size_t n = len;

    mutations_seen++;
    if (dead)
        return -1;
    if (cut) {
        n = (size_t)cut_partial < len ? (size_t)cut_partial : len;
        dead = 1;
    }
    if ((addr >= META_ADDR_A && addr < META_ADDR_A + META_SECTOR_SIZE) ||
        (addr >= META_ADDR_B && addr < META_ADDR_B + META_SECTOR_SIZE)) {
        uint8_t *m = addr >= META_ADDR_B ? &meta_mem[1][addr - META_ADDR_B] : &meta_mem[0][addr - META_ADDR_A];
        for (size_t i = 0; i < n; i++)
            m[i] &= src[i];
        return cut ? -1 : 0;
    }
    s = slot_of_addr(addr, &fb);
    if (s >= 0 && fb && len == IMAGE_HEADER_SIZE && n == len) {
        /* The header is what makes a slot look like an image; it lands only if the write completed.
         * The staged fake image smuggles its class in the header: version at +4, "forged" flag at +12. */
        uint32_t version, forged;
        memcpy(&version, src + 4, 4);
        memcpy(&forged, src + 12, 4);
        slot_content[s] = forged ? C_FORGED : (int)version;
    }
    return cut ? -1 : 0;
}

/* Oracle standing in for the real verifier. */
img_status_t image_check_basic(uint32_t slot_addr_v, uint32_t min_allowed_version, image_header_t *hdr_out)
{
    int s = slot_addr_v == SLOT_B_ADDR, c = slot_content[s];

    if (c == C_INVALID)
        return IMG_ERR_MAGIC;
    if ((uint32_t)content_version(c) < min_allowed_version)
        return IMG_ERR_VERSION_FLOOR;
    if (hdr_out)
        hdr_out->version = (uint32_t)content_version(c);
    return IMG_OK;
}

img_status_t image_check_signature(uint32_t slot_addr_v, const image_header_t *h)
{
    int s = slot_addr_v == SLOT_B_ADDR;
    (void)h;
    return slot_content[s] == C_FORGED ? IMG_ERR_SIGNATURE : IMG_OK;
}

/* ------------------------------------------------------------------ abstract state <-> flash */
typedef struct {
    int valid;
    uint8_t slot, state, trials;
    uint8_t floor;
} mcopy_t;

typedef struct {
    mcopy_t m[2];
    int newer;          /* 0: copy A has the higher seq, 1: copy B (only meaningful when both valid) */
    uint8_t slots[2];
} state_t;

static mcopy_t decode_copy(int c)
{
    boot_metadata_t md;
    mcopy_t r;

    memset(&r, 0, sizeof r);
    memcpy(&md, meta_mem[c], sizeof md);
    if (md.magic != META_MAGIC || md.crc32 != crc32_calc(&md, offsetof(boot_metadata_t, crc32)) ||
        md.active_slot >= SLOT_COUNT || md.boot_state > BOOT_STATE_REVERT_PENDING)
        return r;
    r.valid = 1;
    r.slot = md.active_slot;
    r.state = md.boot_state;
    r.trials = md.trial_count;
    r.floor = (uint8_t)md.min_allowed_version;
    return r;
}

static uint32_t seq_of(int c)
{
    boot_metadata_t md;
    memcpy(&md, meta_mem[c], sizeof md);
    return md.seq;
}

static state_t snapshot(void)
{
    state_t s;

    memset(&s, 0, sizeof s);
    s.m[0] = decode_copy(0);
    s.m[1] = decode_copy(1);
    s.newer = (s.m[0].valid && s.m[1].valid) ? ((int32_t)(seq_of(1) - seq_of(0)) > 0) : (s.m[1].valid ? 1 : 0);
    s.slots[0] = (uint8_t)slot_content[0];
    s.slots[1] = (uint8_t)slot_content[1];
    return s;
}

static void write_copy(int c, const mcopy_t *r, uint32_t seq)
{
    boot_metadata_t md;

    memset(meta_mem[c], 0xFF, META_SECTOR_SIZE);
    if (!r->valid)
        return;
    memset(&md, 0, sizeof md);
    md.magic = META_MAGIC;
    md.seq = seq;
    md.active_slot = r->slot;
    md.boot_state = r->state;
    md.trial_count = r->trials;
    md.min_allowed_version = r->floor;
    md.crc32 = crc32_calc(&md, offsetof(boot_metadata_t, crc32));
    memcpy(meta_mem[c], &md, sizeof md);
}

static void materialise(const state_t *s)
{
    uint32_t sa = s->newer == 0 ? 21 : 20, sb = s->newer == 1 ? 21 : 20;

    write_copy(0, &s->m[0], sa);
    write_copy(1, &s->m[1], sb);
    slot_content[0] = s->slots[0];
    slot_content[1] = s->slots[1];
    op_count = 0;
    cut_at = -1;
    dead = 0;
    mutations_seen = 0;
}

static uint64_t key_of(const state_t *s)
{
    uint64_t k = 0;

    for (int c = 0; c < 2; c++) {
        k = k * 2 + (uint64_t)s->m[c].valid;
        if (s->m[c].valid)
            k = ((k * 2 + s->m[c].slot) * 4 + s->m[c].state) * 8 + s->m[c].trials;
        if (s->m[c].valid)
            k = k * 16 + s->m[c].floor;
        else
            k = k * 16;
    }
    k = k * 2 + (uint64_t)((s->m[0].valid && s->m[1].valid) ? s->newer : 0);
    k = (k * C_KINDS + s->slots[0]) * C_KINDS + s->slots[1];
    return k;
}

static const char *cname(int c)
{
    static const char *n[] = { "-", "v1", "v2", "v3", "FORGED" };
    return n[c];
}

static void print_state(const state_t *s)
{
    for (int c = 0; c < 2; c++) {
        if (s->m[c].valid)
            printf("  meta%c: active=%c state=%d trials=%d floor=%d%s\n", 'A' + c, 'A' + s->m[c].slot, s->m[c].state,
                   s->m[c].trials, s->m[c].floor, (s->m[0].valid && s->m[1].valid && s->newer == c) ? " (newer)" : "");
        else
            printf("  meta%c: invalid\n", 'A' + c);
    }
    printf("  slotA=%s slotB=%s\n", cname(s->slots[0]), cname(s->slots[1]));
}

/* ------------------------------------------------------------------ visited set / queue */
#define HASH_BITS 22
#define QCAP (1u << 21)
static uint64_t *visited;
static state_t *queue;
static size_t qhead, qtail;
static long n_states, n_trans, n_cuts, n_tamper;

static int visit(const state_t *s)
{
    uint64_t k = key_of(s) + 1, h = (k * 0x9E3779B97F4A7C15ull) >> (64 - HASH_BITS);

    while (visited[h]) {
        if (visited[h] == k)
            return 0;
        h = (h + 1) & ((1u << HASH_BITS) - 1);
    }
    visited[h] = k;
    n_states++;
    if (qtail >= QCAP) {
        fprintf(stderr, "queue full\n");
        exit(2);
    }
    queue[qtail++] = *s;
    return 1;
}

/* ------------------------------------------------------------------ environment actions */
enum { A_BOOT_HANG, A_BOOT_CONFIRM, A_BOOT_INSTALL_V1, A_BOOT_INSTALL_V2, A_BOOT_INSTALL_V3, A_BOOT_INSTALL_FORGED, A_COUNT };

static const char *aname(int a)
{
    static const char *n[] = { "boot, app hangs", "boot, app confirms", "boot, app installs v1", "boot, app installs v2",
                               "boot, app installs v3", "boot, app installs FORGED" };
    return n[a];
}

static uint8_t fake_img[IMAGE_HEADER_SIZE + 1024];

static void make_fake_image(int a)
{
    uint32_t version = a == A_BOOT_INSTALL_FORGED ? FORGED_VERSION : (uint32_t)(a - A_BOOT_INSTALL_V1 + 1);
    uint32_t forged = a == A_BOOT_INSTALL_FORGED;

    memset(fake_img, 0xEE, sizeof fake_img);
    memcpy(fake_img, &(uint32_t){ IMAGE_MAGIC }, 4);
    memcpy(fake_img + 4, &version, 4);
    memcpy(fake_img + 12, &forged, 4);
}

typedef struct {
    int booted;            /* boot_decide returned 0 */
    boot_decision_t d;
    int install_rc;
    int confirmed;         /* sf_confirm_healthy() returned success */
    uint32_t floor_seen;   /* floor in force when the decision was taken */
} outcome_t;

static uint32_t newest_floor(int *have)
{
    boot_metadata_t md;
    *have = metadata_load(&md) == 0;
    return *have ? md.min_allowed_version : 0;
}

static outcome_t run_action(int a)
{
    outcome_t o;
    int have;

    memset(&o, 0, sizeof o);
    o.floor_seen = newest_floor(&have);
    memset(&o.d, 0, sizeof o.d);
    o.booted = boot_decide(&o.d) == 0;
    if (!o.booted)
        return o;
    if (a == A_BOOT_CONFIRM) {
        o.confirmed = sf_confirm_healthy() == 0;
    } else if (a >= A_BOOT_INSTALL_V1) {
        make_fake_image(a);
        o.install_rc = sf_install_update(fake_img, sizeof fake_img);
    }
    return o;
}

/* ------------------------------------------------------------------ properties */
static const state_t *cur_state;
static const char *cur_desc;
static char cur_cut[96];

static void violation(const char *prop, const char *msg)
{
    printf("\nVIOLATION %s: %s\n  from state:\n", prop, msg);
    print_state(cur_state);
    printf("  action: %s   %s\n  resulting state:\n", cur_desc, cur_cut);
    {
        state_t s = snapshot();
        print_state(&s);
    }
    exit(1);
}
#define REQUIRE(prop, cond, msg) do { if (!(cond)) violation(prop, msg); } while (0)

/* Is slot s legitimately bootable under the given floor? */
static int bootable(const state_t *st, int s, uint32_t floor)
{
    int c = st->slots[s];
    return c >= C_V1 && c <= C_V3 && (uint32_t)c >= floor;
}

static const mcopy_t *newest(const state_t *st)
{
    if (st->m[0].valid && st->m[1].valid)
        return &st->m[st->newer];
    if (st->m[1].valid)
        return &st->m[1];
    return st->m[0].valid ? &st->m[0] : NULL;
}

static void check_transition(const state_t *before, const outcome_t *o)
{
    state_t after = snapshot();
    const mcopy_t *pre = newest(before), *post = newest(&after);

    if (o->booted) {
        int s = o->d.slot;
        /* S1: only a validly signed image, at or above the floor in force, is ever booted */
        REQUIRE("S1", before->slots[s] >= C_V1 && before->slots[s] <= C_V3, "booted a slot that is not a validly signed image");
        REQUIRE("S1", o->d.version == (uint32_t)before->slots[s], "boot decision disagrees with the slot's actual version");
        REQUIRE("S1", o->d.version >= o->floor_seen, "booted an image below the anti-rollback floor");
        /* S4: the slot the app is running from is never modified by the installer or confirm */
        REQUIRE("S4", after.slots[s] == before->slots[s], "the running slot was modified");
    }
    /* S5: a successful confirm must leave the image confirmed and the floor raised to its version */
    if (o->confirmed) {
        REQUIRE("S5", post && post->state != BOOT_STATE_TRIAL, "confirm reported success but the image is still in trial");
        REQUIRE("S5", post && post->floor >= o->d.version, "confirm reported success but the anti-rollback floor was not raised");
    }
    /* S2: the floor never decreases, whatever happens (including power cuts) */
    REQUIRE("S2", !pre || !post || post->floor >= pre->floor, "anti-rollback floor decreased");
    /* S3: metadata survives: a valid copy before implies a valid copy after */
    REQUIRE("S3", !pre || post, "power cut destroyed every valid metadata copy");
}

/* L1 on a fresh boot from a state, no cuts */
static void check_liveness(const state_t *st)
{
    outcome_t o;
    int have;
    uint32_t floor;
    int exhausted_no_fallback = 0;

    materialise(st);
    floor = newest_floor(&have);
    if (have) {
        const mcopy_t *m = newest(st);
        exhausted_no_fallback = m->state == BOOT_STATE_TRIAL && m->trials >= MAX_TRIALS && !bootable(st, 1 - m->slot, floor);
    }
    cur_state = st;
    cur_desc = "liveness boot";
    cur_cut[0] = 0;
    memset(&o, 0, sizeof o);
    o.booted = boot_decide(&o.d) == 0;
    if (!o.booted) {
        int any = bootable(st, 0, floor) || bootable(st, 1, floor);
        REQUIRE("L1", !any || exhausted_no_fallback, "boot failed although a legitimately bootable image exists");
    }

    /* L2: an app that never confirms must converge: repeated boots end in a stable state or no boot */
    materialise(st);
    cur_desc = "repeated boots, app never confirms";
    for (unsigned i = 0; i < MAX_TRIALS + 4; i++) {
        boot_decision_t d;
        memset(&d, 0, sizeof d);
        if (boot_decide(&d) != 0)
            return;
        if (!d.trial)
            return;
    }
    violation("L2", "an unconfirmed image was booted more than MAX_TRIALS+3 times in a row");
}

/* ------------------------------------------------------------------ exploration */
static void explore(const state_t *st)
{
    cur_state = st;
    for (int a = 0; a < A_COUNT; a++) {
        state_t after;
        outcome_t o;
        int nmut;

        cur_desc = aname(a);

        /* uncut run: learn how many flash mutations the action performs */
        materialise(st);
        cut_at = -1;
        cur_cut[0] = 0;
        o = run_action(a);
        nmut = mutations_seen;
        check_transition(st, &o);
        after = snapshot();
        n_trans++;
        visit(&after);

        /* every power-cut point */
        for (int k = 0; k < nmut; k++) {
            for (int variant = 0; variant < 3; variant++) {
                /* write cuts: 0 bytes, half, all-but-one; erase cuts: the 3 torn patterns (variant % 3) */
                int partial = variant == 0 ? 0 : variant == 1 ? 19 : 11;   /* %3 -> erase patterns 0,1,2 */
                materialise(st);
                cut_at = k;
                cut_partial = partial;
                snprintf(cur_cut, sizeof cur_cut, "[power cut at mutation %d, variant %d]", k, variant);
                o = run_action(a);
                dead = 0;
                cut_at = -1;
                check_transition(st, &o);
                after = snapshot();
                n_trans++;
                n_cuts++;
                visit(&after);
            }
        }
    }

    /* tampering at rest: any slot becomes any content; metadata untouched, no power-cut variants */
    for (int slot = 0; slot < 2; slot++) {
        for (int c = 0; c < C_KINDS; c++) {
            state_t t = *st;
            if (t.slots[slot] == c)
                continue;
            t.slots[slot] = (uint8_t)c;
            n_trans++;
            n_tamper++;
            visit(&t);
        }
    }
    check_liveness(st);
}

int main(void)
{
    state_t init;

    visited = calloc((size_t)1 << HASH_BITS, sizeof *visited);
    queue = malloc(QCAP * sizeof *queue);
    if (!visited || !queue)
        return 2;

    /* Initial states: a factory device (nothing but slot A, blank metadata) and a blank device. */
    memset(&init, 0, sizeof init);
    init.slots[0] = C_V1;
    visit(&init);
    memset(&init, 0, sizeof init);
    visit(&init);

    while (qhead < qtail) {
        state_t s = queue[qhead++];
        explore(&s);
        if ((qhead & 0x3FFF) == 0)
            fprintf(stderr, "  ... %zu explored, %ld states known, %ld transitions\n", qhead, n_states, n_trans);
    }
    printf("model check complete: %ld reachable device states, %ld transitions (%ld with a power cut, %ld tampering), "
           "all properties hold\n", n_states, n_trans, n_cuts, n_tamper);
    return 0;
}
