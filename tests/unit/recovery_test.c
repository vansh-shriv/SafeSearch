/*
 * Recovery-mode tests with real signatures: the production recovery.c + boot_logic.c + crypto against a mock
 * flash. Needs build/app_v1_slotA.img, build/app_v2_slotB.img, build/pubkey.c from `make`.
 */
#include "mock_flash.h"
#include "crc32.h"
#include "recovery_env.h"
#include "boot_logic.h"
#include "flash_hal.h"
#include "flash_map.h"
#include "image_verify.h"
#include "metadata.h"
#include "recovery.h"
#include "safeflash_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void boot_puts(const char *s) { (void)s; }
void boot_puthex(uint32_t v) { (void)v; }

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint8_t v1[SLOT_SIZE], v2[SLOT_SIZE];
static size_t v1_len, v2_len;
static uint8_t stream[SLOT_SIZE * 2];
static uint8_t altered[SLOT_SIZE * 2];

static size_t load(const char *p, uint8_t *dst, size_t cap)
{
    FILE *f = fopen(p, "rb");
    size_t n;
    if (!f) { fprintf(stderr, "cannot open %s (run `mingw32-make` first)\n", p); exit(2); }
    n = fread(dst, 1, cap, f);
    fclose(f);
    return n;
}

/* A device that has booted v1 from slot A once (metadata valid, floor 1), then loses both images. */
static void bricked_device(void)
{
    boot_decision_t d;
    mock_flash_reset();
    flash_write(SLOT_A_ADDR + IMAGE_HEADER_SIZE, v1 + IMAGE_HEADER_SIZE, v1_len - IMAGE_HEADER_SIZE);
    flash_write(SLOT_A_ADDR, v1, IMAGE_HEADER_SIZE);
    memset(&d, 0, sizeof d);
    boot_decide(&d);
    sf_confirm_healthy();                                   /* floor 1 */
    flash_erase_sector(SLOT_A_FIRST_SECTOR);                /* slot A wiped: nothing bootable */
}

static int run_stream(const uint8_t *s, size_t n)
{
    rec_env_set_input(s, n);
    return recovery_run();
}

static void test_happy_path(void)
{
    boot_decision_t d;
    size_t n;

    puts("recovery: full transfer installs and boots");
    bricked_device();
    memset(&d, 0, sizeof d);
    CHECK(boot_decide(&d) != 0);                            /* precondition: nothing bootable */

    n = rec_stream_from_image(stream, v2, v2_len);
    CHECK(run_stream(stream, n) == RECOVERY_INSTALLED);
    {
        rec_replies_t r = rec_env_parse_replies();
        CHECK(r.naks == 0 && r.acks == 1 + (unsigned)((v2_len + 255) / 256) + 1);
        CHECK(r.last_ack_offset == v2_len);
    }
    memset(&d, 0, sizeof d);
    CHECK(boot_decide(&d) == 0 && d.slot == 1 && d.version == 2 && d.trial);
}

static void test_noise_duplicates_and_bad_crc(void)
{
    /* garbage before frames, a corrupted-CRC frame, and every DATA frame sent twice must all be tolerated */
    size_t n = rec_stream_from_image(stream, v2, v2_len), i = 0, o = 0;
    boot_decision_t d;

    puts("recovery: noise, duplicate frames and a bad CRC are tolerated");
    for (int k = 0; k < 40; k++)
        altered[o++] = (uint8_t)(k * 37);                   /* line noise (may contain 0xA5, must resync) */
    while (i < n) {
        uint16_t len = (uint16_t)(stream[i + 2] | (stream[i + 3] << 8));
        size_t fl = 8u + len;
        if (stream[i + 1] == REC_DATA && i > 200 && i < 700) {   /* corrupt this frame once, then send it properly */
            memcpy(altered + o, stream + i, fl);
            altered[o + 6] ^= 0x40;
            o += fl;
        }
        memcpy(altered + o, stream + i, fl);
        o += fl;
        if (stream[i + 1] == REC_DATA) {                    /* duplicate every DATA frame */
            memcpy(altered + o, stream + i, fl);
            o += fl;
        }
        i += fl;
    }
    bricked_device();
    CHECK(run_stream(altered, o) == RECOVERY_INSTALLED);
    {
        rec_replies_t r = rec_env_parse_replies();
        CHECK(r.naks >= 1);                                 /* the corrupted frame was refused */
    }
    memset(&d, 0, sizeof d);
    CHECK(boot_decide(&d) == 0 && d.version == 2);
}

static void test_forged_image_rejected(void)
{
    boot_metadata_t before, after;
    size_t n;
    uint8_t forged[SLOT_SIZE];
    boot_decision_t d;

    puts("recovery: forged image is rejected and nothing is committed");
    memcpy(forged, v2, v2_len);
    forged[IMAGE_HEADER_SIZE + 10] ^= 1;                    /* payload changed, signed hash now stale */
    /* keep the unkeyed CRCs consistent so only the crypto can catch it */
    {
        image_header_t *h = (image_header_t *)forged;
        h->image_crc32 = crc32_calc(forged + IMAGE_HEADER_SIZE, h->image_size);
        h->header_crc32 = crc32_calc(h, offsetof(image_header_t, header_crc32));
    }
    bricked_device();
    metadata_load(&before);
    n = rec_stream_from_image(stream, forged, v2_len);
    CHECK(run_stream(stream, n) == RECOVERY_EOF);           /* never reports success */
    {
        rec_replies_t r = rec_env_parse_replies();
        CHECK(r.last_nak_reason == REC_NAK_BAD_IMAGE);
        CHECK(r.last_nak_detail == IMG_ERR_HASH);
    }
    metadata_load(&after);
    CHECK(after.seq == before.seq && after.active_slot == before.active_slot);
    memset(&d, 0, sizeof d);
    CHECK(boot_decide(&d) != 0);                            /* still nothing bootable */
}

static void test_rollback_rejected(void)
{
    size_t n;

    puts("recovery: a validly signed but older image is refused (anti-rollback)");
    bricked_device();                                       /* floor is 1 */
    {   /* raise the floor to 2 as if v2 had been confirmed earlier */
        boot_metadata_t md;
        metadata_load(&md);
        md.min_allowed_version = 2;
        metadata_store(&md);
    }
    n = rec_stream_from_image(stream, v1, v1_len);          /* genuine, correctly signed v1 */
    CHECK(run_stream(stream, n) == RECOVERY_EOF);
    {
        rec_replies_t r = rec_env_parse_replies();
        CHECK(r.last_nak_reason == REC_NAK_BAD_IMAGE && r.last_nak_detail == IMG_ERR_VERSION_FLOOR);
    }
}

static void test_protocol_errors(void)
{
    uint8_t p[8], *o = altered;
    size_t n = 0;
    rec_replies_t r;

    puts("recovery: protocol violations are NAKed");
    bricked_device();
    /* DATA before BEGIN */
    p[0] = p[1] = p[2] = p[3] = 0;
    n += recovery_build_frame(o + n, REC_DATA, p, 8);
    rec_env_set_input(o, n);
    recovery_run();
    r = rec_env_parse_replies();
    CHECK(r.naks == 1 && r.last_nak_reason == REC_NAK_STATE);

    /* absurd BEGIN size, then a valid BEGIN followed by a skipped offset */
    n = 0;
    p[0] = p[1] = p[2] = 0xFF; p[3] = 0x7F;
    n += recovery_build_frame(o + n, REC_BEGIN, p, 4);
    rec_env_set_input(o, n);
    recovery_run();
    r = rec_env_parse_replies();
    CHECK(r.naks == 1 && r.last_nak_reason == REC_NAK_SIZE);

    n = 0;
    p[0] = 0x00; p[1] = 0x0A; p[2] = 0; p[3] = 0;           /* 2560 bytes */
    n += recovery_build_frame(o + n, REC_BEGIN, p, 4);
    p[0] = 0x00; p[1] = 0x02; p[2] = 0; p[3] = 0;           /* offset 512 before 0 */
    {
        uint8_t data[4 + 256];
        memcpy(data, p, 4);
        memset(data + 4, 0xAB, 256);
        n += recovery_build_frame(o + n, REC_DATA, data, sizeof data);
    }
    rec_env_set_input(o, n);
    recovery_run();
    r = rec_env_parse_replies();
    CHECK(r.acks == 1 && r.naks == 1 && r.last_nak_reason == REC_NAK_OFFSET);

    /* unknown frame type */
    n = recovery_build_frame(o, 0x55, NULL, 0);
    rec_env_set_input(o, n);
    recovery_run();
    r = rec_env_parse_replies();
    CHECK(r.naks == 1 && r.last_nak_reason == REC_NAK_TYPE);
}

static void test_header_written_last(void)
{
    size_t n, cut;

    puts("recovery: an interrupted transfer never leaves a valid-looking header");
    bricked_device();
    n = rec_stream_from_image(stream, v2, v2_len);
    /* drop the END frame (and a bit more): the stream stops before the header would be written */
    cut = n - 8;
    rec_env_set_input(stream, cut);
    CHECK(recovery_run() == RECOVERY_EOF);
    {
        uint8_t magic[4];
        flash_read(SLOT_B_ADDR, magic, 4);
        CHECK(magic[0] == 0xFF && magic[1] == 0xFF && magic[2] == 0xFF && magic[3] == 0xFF);
        CHECK(image_check_basic(SLOT_B_ADDR, 0, NULL) != IMG_OK);
    }
}

int main(void)
{
    v1_len = load("../../build/app_v1_slotA.img", v1, sizeof v1);
    v2_len = load("../../build/app_v2_slotB.img", v2, sizeof v2);

    test_happy_path();
    test_noise_duplicates_and_bad_crc();
    test_forged_image_rejected();
    test_rollback_rejected();
    test_protocol_errors();
    test_header_written_last();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
