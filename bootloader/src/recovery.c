#include "recovery.h"
#include "boot_logic.h"
#include "crc32.h"
#include "flash_hal.h"
#include "flash_map.h"
#include "image_crypto.h"
#include "image_verify.h"
#include "metadata.h"

#define SECTOR_BYTES 0x20000u   /* slot sectors are all 128 KB on the STM32F4 */

static uint8_t rx[REC_MAX_PAYLOAD];
static uint8_t hdr_buf[IMAGE_HEADER_SIZE];   /* first 1 KB of the image, written last */

/* transfer state */
static int active;            /* BEGIN accepted, transfer in progress */
static uint8_t target;
static uint32_t total_len;
static uint32_t next_off;

uint32_t recovery_build_frame(uint8_t *buf, uint8_t type, const uint8_t *payload, uint16_t len)
{
    uint32_t crc;

    buf[0] = REC_SOF;
    buf[1] = type;
    buf[2] = (uint8_t)len;
    buf[3] = (uint8_t)(len >> 8);
    for (uint16_t i = 0; i < len; i++)
        buf[4 + i] = payload[i];
    crc = crc32_calc(buf + 1, 3u + len);
    for (int i = 0; i < 4; i++)
        buf[4 + len + i] = (uint8_t)(crc >> (8 * i));
    return 8u + len;
}

static void send_frame(uint8_t type, const uint8_t *payload, uint16_t len)
{
    uint8_t buf[8 + 8];
    uint32_t n = recovery_build_frame(buf, type, payload, len);

    for (uint32_t i = 0; i < n; i++)
        recovery_putc(buf[i]);
}

static void ack(void)
{
    const uint8_t p[4] = { (uint8_t)next_off, (uint8_t)(next_off >> 8), (uint8_t)(next_off >> 16), (uint8_t)(next_off >> 24) };

    send_frame(REC_ACK, p, 4);
}

static void nak(uint8_t reason, uint8_t detail)
{
    const uint8_t p[2] = { reason, detail };

    send_frame(REC_NAK, p, reason == REC_NAK_BAD_IMAGE ? 2 : 1);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Read exactly n bytes into dst; returns 0, or -1 if the input ended. */
static int read_bytes(uint8_t *dst, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        int c = recovery_getc();
        if (c < 0)
            return -1;
        dst[i] = (uint8_t)c;
    }
    return 0;
}

static void reset_transfer(void)
{
    active = 0;
    total_len = 0;
    next_off = 0;
}

/* Which slot to write: the one that is not the current active slot (slot A when no metadata exists). */
static uint8_t pick_target(void)
{
    boot_metadata_t md;

    return metadata_load(&md) == 0 ? (uint8_t)(1 - md.active_slot) : 0;
}

static void on_begin(const uint8_t *p, uint16_t len)
{
    uint32_t sectors, first;

    if (len != 4) {
        nak(REC_NAK_LEN, 0);
        return;
    }
    total_len = rd32(p);
    if (total_len <= IMAGE_HEADER_SIZE || total_len > SLOT_SIZE) {
        reset_transfer();
        nak(REC_NAK_SIZE, 0);
        return;
    }
    target = pick_target();
    first = target == 0 ? SLOT_A_FIRST_SECTOR : SLOT_B_FIRST_SECTOR;
    sectors = (total_len + SECTOR_BYTES - 1) / SECTOR_BYTES;
    active = 0;   /* not accepting data until the slot is erased */
    for (uint32_t i = 0; i < sectors; i++) {
        if (flash_erase_sector(first + i) != 0) {
            reset_transfer();
            nak(REC_NAK_FLASH, 0);
            return;
        }
    }
    for (uint32_t i = 0; i < IMAGE_HEADER_SIZE; i++)
        hdr_buf[i] = 0xFF;
    next_off = 0;
    active = 1;
    ack();
}

static void on_data(const uint8_t *p, uint16_t len)
{
    uint32_t off, n;

    if (!active) {
        nak(REC_NAK_STATE, 0);
        return;
    }
    if (len < 5 || len > REC_MAX_PAYLOAD) {
        nak(REC_NAK_LEN, 0);
        return;
    }
    off = rd32(p);
    n = (uint32_t)len - 4u;
    if (off < next_off) {   /* retransmit of something already stored: idempotent */
        ack();
        return;
    }
    /* in order, aligned chunks; only the final chunk may be short */
    if (off != next_off || (off % REC_MAX_DATA) != 0 || off + n > total_len ||
        (n != REC_MAX_DATA && off + n != total_len)) {
        nak(REC_NAK_OFFSET, 0);
        return;
    }
    if (off < IMAGE_HEADER_SIZE) {   /* chunks never straddle the header boundary (256 divides 1024) */
        for (uint32_t i = 0; i < n; i++)
            hdr_buf[off + i] = p[4 + i];
    } else if (flash_write(slot_addr(target) + off, p + 4, n) != 0) {
        reset_transfer();
        nak(REC_NAK_FLASH, 0);
        return;
    }
    next_off = off + n;
    ack();
}

/* Returns 1 when an image was installed. */
static int on_end(uint16_t len)
{
    boot_metadata_t md;
    image_header_t h;
    img_status_t s;
    uint32_t floor = 0;

    if (!active || len != 0) {
        nak(active ? REC_NAK_LEN : REC_NAK_STATE, 0);
        return 0;
    }
    if (next_off != total_len) {
        nak(REC_NAK_OFFSET, 0);
        return 0;
    }
    /* Header last: only now does the slot get a header, so a partial transfer never looks valid. */
    if (flash_write(slot_addr(target), hdr_buf, IMAGE_HEADER_SIZE) != 0) {
        reset_transfer();
        nak(REC_NAK_FLASH, 0);
        return 0;
    }
    if (metadata_load(&md) == 0)
        floor = md.min_allowed_version;
    else {   /* no valid metadata at all: start a fresh one (the anti-rollback floor is lost in that case) */
        md.active_slot = 0;
        md.trial_count = 0;
        md.reserved = 0;
        md.min_allowed_version = 0;
    }
    s = image_check_basic(slot_addr(target), floor, &h);
    if (s == IMG_OK)
        s = image_check_signature(slot_addr(target), &h);
    if (s != IMG_OK) {
        reset_transfer();
        nak(REC_NAK_BAD_IMAGE, (uint8_t)s);
        boot_puts("BL: recovery image rejected\n");
        return 0;
    }
    md.active_slot = target;
    md.boot_state = BOOT_STATE_TRIAL;
    md.trial_count = 0;
    if (metadata_store(&md) != 0) {
        reset_transfer();
        nak(REC_NAK_FLASH, 0);
        return 0;
    }
    ack();
    reset_transfer();
    return 1;
}

int recovery_run(void)
{
    uint8_t hdr[3];
    uint8_t crcb[4];

    reset_transfer();
    for (;;) {
        int c;
        uint16_t len;
        uint32_t crc;

        /* resync: skip everything up to the next start-of-frame byte */
        do {
            c = recovery_getc();
            if (c < 0)
                return RECOVERY_EOF;
        } while (c != (int)REC_SOF);

        if (read_bytes(hdr, 3) != 0)
            return RECOVERY_EOF;
        len = (uint16_t)(hdr[1] | (hdr[2] << 8));
        if (len > REC_MAX_PAYLOAD) {
            nak(REC_NAK_LEN, 0);
            continue;
        }
        if (read_bytes(rx, len) != 0 || read_bytes(crcb, 4) != 0)
            return RECOVERY_EOF;

        crc = crc32_update(0, hdr, 3);
        crc = crc32_update(crc, rx, len);
        if (crc != rd32(crcb)) {
            nak(REC_NAK_CRC, 0);
            continue;
        }

        switch (hdr[0]) {
        case REC_BEGIN: on_begin(rx, len); break;
        case REC_DATA:  on_data(rx, len); break;
        case REC_END:
            if (on_end(len))
                return RECOVERY_INSTALLED;
            break;
        case REC_ABORT: reset_transfer(); ack(); break;
        default: nak(REC_NAK_TYPE, 0); break;
        }
    }
}
