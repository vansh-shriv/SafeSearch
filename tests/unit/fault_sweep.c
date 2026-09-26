/*
 * Exhaustive power-cut sweep over the REAL update / confirm / boot code paths.
 *
 * The code under test is the production source (boot_logic.c, safeflash_update.c, safeflash_app.c,
 * metadata.c, image_verify.c, image_crypto.c with real SHA-256 + ECDSA) running against a mock flash.
 * For every flash mutation an operation performs, and for every byte boundary inside each write and
 * every torn-erase state, power is cut at that exact point; then the device boots and we assert:
 *   - it always boots some signature-verified image (never bricked),
 *   - that image is the old one or the new one, nothing else,
 *   - the boot metadata is valid afterwards and the anti-rollback floor never exceeds the running version,
 *   - the system converges (an interrupted update either completes into a confirmed image or reverts).
 * Every (cut point -> outcome) pair is written to a CSV.
 *
 * Needs build/app_v1_slotA.img, build/app_v2_slotB.img and build/pubkey.c from `make`.
 */
#include "mock_flash.h"
#include "boot_config.h"
#include "boot_logic.h"
#include "flash_hal.h"
#include "flash_map.h"
#include "image_verify.h"
#include "metadata.h"
#include "safeflash_app.h"
#include "recovery.h"
#include "recovery_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- environment hooks for boot_logic.c: silent ---- */
void boot_puts(const char *s) { (void)s; }
void boot_puthex(uint32_t v) { (void)v; }

static uint8_t v1_img[SLOT_SIZE], v2_img[SLOT_SIZE];
static size_t v1_len, v2_len;
static uint8_t golden_after_factory[MOCK_FLASH_SIZE];
static uint8_t snap[MOCK_FLASH_SIZE];

static FILE *csv;
static long total_points, total_bad;

static size_t load_file(const char *path, uint8_t *dst, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s (run `mingw32-make` first)\n", path);
        exit(2);
    }
    size_t n = fread(dst, 1, cap, f);
    fclose(f);
    return n;
}

static int boot_once(boot_decision_t *d)
{
    memset(d, 0, sizeof *d);
    return boot_decide(d);
}

/* Fresh device: factory v1 in slot A, metadata initialised by a first boot. Saves state in golden. */
static void make_factory_device(void)
{
    boot_decision_t d;
    mock_flash_reset();
    flash_write(SLOT_A_ADDR + IMAGE_HEADER_SIZE, v1_img + IMAGE_HEADER_SIZE, v1_len - IMAGE_HEADER_SIZE);
    flash_write(SLOT_A_ADDR, v1_img, IMAGE_HEADER_SIZE);
    if (boot_once(&d) != 0 || d.slot != 0 || d.trial) {
        fprintf(stderr, "factory boot failed\n");
        exit(2);
    }
    (void)sf_confirm_healthy();   /* v1 app runs and ratchets the floor to 1 */
    boot_metadata_t md;
    if (metadata_load(&md) != 0 || md.min_allowed_version != 1) {
        fprintf(stderr, "factory ratchet failed\n");
        exit(2);
    }
    mock_flash_save(golden_after_factory);
}

/* App confirms after boot when it is in trial, as the real app does. */
static void app_runs_and_confirms(const boot_decision_t *d)
{
    if (d->trial)
        (void)sf_confirm_healthy();
}

typedef struct {
    const char *scenario;
    long points, bad;
} tally_t;

static void record(tally_t *t, int op, const char *kind, size_t partial, const boot_decision_t *d,
                   int rc, int ok, const char *why)
{
    t->points++;
    total_points++;
    if (!ok) {
        t->bad++;
        total_bad++;
        printf("  VIOLATION %s op=%d %s partial=%zu: %s (rc=%d slot=%d ver=%u)\n", t->scenario, op, kind, partial,
               why, rc, d ? d->slot : -1, d ? d->version : 0);
    }
    fprintf(csv, "%s,%d,%s,%zu,%d,%d,%u,%d,%s\n", t->scenario, op, kind, partial, rc, d ? d->slot : -1,
            d ? d->version : 0, ok, ok ? "recovered" : why);
}

/* Common post-cut assertions after power comes back and the device boots once. */
static uint32_t pre_floor;   /* floor in the pre-op state; must never decrease */
static int allow_unbootable;   /* recovery scenario: no bootable image after a cut is legal if recovery can resume */

static int verify_recovered(const boot_decision_t *d, int rc, uint32_t v_old, uint32_t v_new, const char **why)
{
    boot_metadata_t md;

    if (rc != 0 && allow_unbootable) {
        /* Nothing bootable is acceptable here (recovery mode would run) but metadata must still be intact. */
        if (metadata_load(&md) != 0) { *why = "no valid metadata while unbootable"; return 0; }
        return 1;
    }
    if (rc != 0) { *why = "device did not boot (bricked)"; return 0; }
    if (d->version != v_old && d->version != v_new) { *why = "booted unexpected version"; return 0; }
    if (metadata_load(&md) != 0) { *why = "no valid metadata after boot"; return 0; }
    if (md.min_allowed_version > d->version) { *why = "floor above running version"; return 0; }
    if (md.min_allowed_version < pre_floor) { *why = "anti-rollback floor decreased"; return 0; }
    return 1;
}

/* ------------------------------------------------------------------------------------------------
 * Scenario driver: given a prepared flash snapshot and an operation, enumerate every cut point.
 * ---------------------------------------------------------------------------------------------- */
typedef void (*op_fn)(void);

static void sweep(tally_t *t, const uint8_t *pre_state, op_fn op, uint32_t v_old, uint32_t v_new,
                  int (*post_check)(const boot_decision_t *, int, const char **))
{
    int nops;

    /* Reference run without a cut to learn the mutation trace. */
    mock_flash_restore(pre_state);
    {
        boot_metadata_t pm;
        pre_floor = metadata_load(&pm) == 0 ? pm.min_allowed_version : 0;
    }
    op();
    nops = mock_flash_mutation_count();
    static mock_op_kind_t kinds[MOCK_MAX_OPS];
    static size_t lens[MOCK_MAX_OPS];
    for (int i = 0; i < nops; i++) {
        kinds[i] = mock_flash_op_kind(i);
        lens[i] = mock_flash_op_len(i);
    }

    for (int k = 0; k <= nops; k++) {
        size_t nparts = k == nops ? 1 : (kinds[k] == MOCK_OP_ERASE ? 3 : lens[k]);
        const char *kind = k == nops ? "none" : (kinds[k] == MOCK_OP_ERASE ? "erase" : "write");
        for (size_t partial = 0; partial < nparts; partial++) {
            boot_decision_t d;
            const char *why = "";

            mock_flash_restore(pre_state);
            if (k < nops)
                mock_flash_arm_cut(k, partial);
            op();                       /* dies at the cut (or completes for k == nops) */
            mock_flash_power_on();

            int rc = boot_once(&d);
            int ok = verify_recovered(&d, rc, v_old, v_new, &why);
            if (ok && post_check)
                ok = post_check(&d, rc, &why);
            record(t, k, kind, partial, &d, rc, ok, why);
        }
    }
}

/* ---- scenario 1: OTA install of v2 over running v1 ---- */
static void op_install(void) { (void)sf_install_update(v2_img, v2_len); }

/* After a cut the app (if it was a trial) confirms; the following boot must be stable and confirmed. */
static int post_install(const boot_decision_t *d, int rc, const char **why)
{
    (void)rc;
    boot_decision_t d2;
    boot_metadata_t md;

    app_runs_and_confirms(d);
    if (boot_once(&d2) != 0 || d2.slot != d->slot || d2.version != d->version || d2.trial) {
        *why = "not stable/confirmed after app confirm";
        return 0;
    }
    if (metadata_load(&md) != 0 || md.min_allowed_version > d2.version ||
        (d->version == 2 && md.min_allowed_version != 2)) {
        *why = "ratchet wrong after confirm";
        return 0;
    }
    /* Invariant for a v1 outcome: old image still intact and the update was fully abandoned. */
    if (d->version == 1 && d->slot != 0) {
        *why = "v1 booted from wrong slot";
        return 0;
    }
    return 1;
}

/* ---- scenario 2: app confirms a trial image (metadata write interrupted) ---- */
static uint8_t trial_state[MOCK_FLASH_SIZE];

static void prepare_trial_state(void)
{
    boot_decision_t d;
    mock_flash_restore(golden_after_factory);
    if (sf_install_update(v2_img, v2_len) != SF_OK || boot_once(&d) != 0 || !d.trial || d.slot != 1) {
        fprintf(stderr, "cannot prepare trial state\n");
        exit(2);
    }
    mock_flash_save(trial_state);   /* B in TRIAL, trial_count 1 (as after the first trial boot) */
}

static void op_confirm(void) { (void)sf_confirm_healthy(); }

/* After an interrupted confirm the app never confirms again: the device must converge on its own. */
static int post_confirm(const boot_decision_t *d, int rc, const char **why)
{
    (void)rc;
    boot_decision_t cur = *d;

    for (int i = 0; i < (int)MAX_TRIALS + 2 && cur.trial; i++) {
        if (boot_once(&cur) != 0) {
            *why = "bricked while retrying trials";
            return 0;
        }
    }
    if (cur.trial) {
        *why = "never converged out of trial";
        return 0;
    }
    if (cur.version != 1 && cur.version != 2) {
        *why = "converged on unexpected version";
        return 0;
    }
    return 1;
}

/* ---- scenario 3: cuts inside the bootloader's own metadata writes ---- */
static void op_boot(void)
{
    boot_decision_t d;
    (void)boot_once(&d);
}

static int post_boot_next(const boot_decision_t *d, int rc, const char **why)
{
    (void)d; (void)rc; (void)why;
    return 1;
}


/* ---- scenario 4: cuts inside a serial-recovery install onto a device with nothing bootable ---- */
static uint8_t rec_stream_buf[SLOT_SIZE * 2];
static size_t rec_stream_len;
static uint8_t bricked_state[MOCK_FLASH_SIZE];

static void op_recovery(void)
{
    rec_env_set_input(rec_stream_buf, rec_stream_len);
    (void)recovery_run();
}

static int post_recovery(const boot_decision_t *d, int rc, const char **why)
{
    boot_decision_t d2;

    if (rc == 0) {   /* something boots after the cut: it can only be the new, verified v2 in slot B */
        if (d->version != 2 || d->slot != 1) { *why = "booted something other than the verified new image"; return 0; }
        return 1;
    }
    /* nothing bootable: recovery mode would run again, and a full transfer must succeed from this state */
    rec_env_set_input(rec_stream_buf, rec_stream_len);
    if (recovery_run() != RECOVERY_INSTALLED) { *why = "recovery cannot resume after the cut"; return 0; }
    if (boot_once(&d2) != 0 || d2.version != 2 || d2.slot != 1) { *why = "device not bootable after resumed recovery"; return 0; }
    return 1;
}

int main(void)
{
    tally_t t;
    char p1[] = "../../build/app_v1_slotA.img";
    char p2[] = "../../build/app_v2_slotB.img";

    v1_len = load_file(p1, v1_img, sizeof v1_img);
    v2_len = load_file(p2, v2_img, sizeof v2_img);
    csv = fopen("../../build/fault_sweep.csv", "w");
    if (!csv) { fprintf(stderr, "cannot write csv\n"); return 2; }
    fprintf(csv, "scenario,op_index,op_kind,partial,boot_rc,boot_slot,boot_version,pass,outcome\n");

    make_factory_device();

    /* 1. Cut anywhere during the OTA install. */
    t = (tally_t){ "ota_install", 0, 0 };
    printf("ota_install: cutting every flash mutation of sf_install_update (v1 running, v2 delivered)\n");
    sweep(&t, golden_after_factory, op_install, 1, 2, post_install);
    printf("  %ld cut points, %ld violations\n", t.points, t.bad);

    /* 2. Cut anywhere during confirm (trial -> confirmed + ratchet). */
    prepare_trial_state();
    t = (tally_t){ "confirm", 0, 0 };
    printf("confirm: cutting every flash mutation of sf_confirm_healthy (v2 in trial)\n");
    sweep(&t, trial_state, op_confirm, 1, 2, post_confirm);
    printf("  %ld cut points, %ld violations\n", t.points, t.bad);

    /* 3a. Bootloader first boot: metadata initialisation interrupted. Blank metadata, factory v1. */
    mock_flash_restore(golden_after_factory);
    memset(mock_flash_raw(META_ADDR_A), 0xFF, META_SECTOR_SIZE);
    memset(mock_flash_raw(META_ADDR_B), 0xFF, META_SECTOR_SIZE);
    mock_flash_save(snap);
    t = (tally_t){ "boot_first_boot", 0, 0 };
    printf("boot_first_boot: cutting every mutation while the bootloader initialises metadata\n");
    sweep(&t, snap, op_boot, 1, 1, post_boot_next);
    printf("  %ld cut points, %ld violations\n", t.points, t.bad);

    /* 3b. Bootloader trial accounting: B in trial, count 0 -> increment interrupted. */
    mock_flash_restore(golden_after_factory);
    (void)sf_install_update(v2_img, v2_len);
    mock_flash_save(snap);
    t = (tally_t){ "boot_trial_increment", 0, 0 };
    printf("boot_trial_increment: cutting every mutation of the trial-counter update\n");
    sweep(&t, snap, op_boot, 1, 2, post_boot_next);
    printf("  %ld cut points, %ld violations\n", t.points, t.bad);

    /* 3c. Bootloader revert: B in trial with count == MAX_TRIALS -> revert write interrupted. */
    {
        boot_metadata_t md;
        boot_decision_t d;
        mock_flash_restore(golden_after_factory);
        (void)sf_install_update(v2_img, v2_len);
        for (unsigned i = 0; i < MAX_TRIALS; i++)
            (void)boot_once(&d);            /* consume all trial attempts, app never confirms */
        metadata_load(&md);
        if (md.trial_count != MAX_TRIALS || md.boot_state != BOOT_STATE_TRIAL) {
            fprintf(stderr, "cannot prepare revert state\n");
            return 2;
        }
        mock_flash_save(snap);
    }
    t = (tally_t){ "boot_revert", 0, 0 };
    printf("boot_revert: cutting every mutation of the revert-to-old-slot commit\n");
    sweep(&t, snap, op_boot, 1, 1, post_boot_next);   /* the only legal end state is v1 */
    printf("  %ld cut points, %ld violations\n", t.points, t.bad);

    /* 4. Recovery install onto a device that lost both images. */
    mock_flash_restore(golden_after_factory);
    (void)flash_erase_sector(SLOT_A_FIRST_SECTOR);      /* slot A wiped: nothing bootable */
    mock_flash_save(bricked_state);
    rec_stream_len = rec_stream_from_image(rec_stream_buf, v2_img, v2_len);
    t = (tally_t){ "recovery_install", 0, 0 };
    printf("recovery_install: cutting every flash mutation of a serial recovery install\n");
    allow_unbootable = 1;
    sweep(&t, bricked_state, op_recovery, 2, 2, post_recovery);
    allow_unbootable = 0;
    printf("  %ld cut points, %ld violations\n", t.points, t.bad);

    fclose(csv);
    printf("\nTOTAL: %ld distinct fault points, %ld violations (%ld recovered correctly)\n", total_points,
           total_bad, total_points - total_bad);
    printf("results: build/fault_sweep.csv\n");
    return total_bad != 0;
}
