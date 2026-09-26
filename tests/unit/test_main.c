#include "mock_flash.h"
#include "crc32.h"
#include "metadata.h"
#include "image_verify.h"
#include "flash_map.h"
#include "flash_hal.h"
#include "sha256.h"

#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static boot_metadata_t mk(uint8_t slot, uint8_t state, uint8_t trials, uint32_t floor)
{
    boot_metadata_t m;
    memset(&m, 0, sizeof m);
    m.active_slot = slot;
    m.boot_state = state;
    m.trial_count = trials;
    m.min_allowed_version = floor;
    return m;
}

static void test_crc32(void)
{
    puts("crc32");
    CHECK(crc32_calc("123456789", 9) == 0xCBF43926u);
    CHECK(crc32_calc("", 0) == 0);
    CHECK(crc32_update(crc32_update(0, "1234", 4), "56789", 5) == 0xCBF43926u);
}

static int sha_is(const void *msg, size_t len, const char *hex)
{
    sha256_ctx c;
    uint8_t d[32];
    char s[65];
    sha256_init(&c);
    /* feed in odd-sized pieces to exercise buffering */
    const uint8_t *p = msg;
    for (size_t off = 0; off < len;) {
        size_t n = (off % 7) + 1;
        if (n > len - off) n = len - off;
        sha256_update(&c, p + off, n);
        off += n;
    }
    sha256_final(&c, d);
    for (int i = 0; i < 32; i++) sprintf(s + 2 * i, "%02x", d[i]);
    return strcmp(s, hex) == 0;
}

static void test_sha256(void)
{
    puts("sha256");
    CHECK(sha_is("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK(sha_is("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK(sha_is("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
                 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    static uint8_t million[1000000];
    memset(million, 'a', sizeof million);
    CHECK(sha_is(million, sizeof million, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

static void test_metadata_basic(void)
{
    puts("metadata basic");
    boot_metadata_t m, out, a, b;
    mock_flash_reset();
    CHECK(metadata_load(&out) == -1);

    m = mk(0, BOOT_STATE_NORMAL, 0, 1);
    CHECK(metadata_store(&m) == 0);
    CHECK(metadata_load(&out) == 0 && out.seq == 1 && out.active_slot == 0);

    m = mk(1, BOOT_STATE_TRIAL, 1, 1);
    CHECK(metadata_store(&m) == 0);
    CHECK(metadata_load(&out) == 0 && out.seq == 2 && out.active_slot == 1);

    /* ping-pong: seq 1 in copy A, seq 2 in copy B, seq 3 back in A */
    flash_read(META_ADDR_A, &a, sizeof a);
    flash_read(META_ADDR_B, &b, sizeof b);
    CHECK(a.seq == 1 && b.seq == 2);
    m = mk(1, BOOT_STATE_CONFIRMED, 0, 2);
    metadata_store(&m);
    flash_read(META_ADDR_A, &a, sizeof a);
    CHECK(a.seq == 3);
}

static void test_metadata_corruption(void)
{
    puts("metadata corruption falls back to older copy");
    boot_metadata_t m, out;
    mock_flash_reset();
    m = mk(0, BOOT_STATE_NORMAL, 0, 1); metadata_store(&m);   /* seq1 -> A */
    m = mk(1, BOOT_STATE_TRIAL, 1, 1);  metadata_store(&m);   /* seq2 -> B */

    mock_flash_raw(META_ADDR_B)[5] ^= 0x01;                   /* corrupt newest */
    CHECK(metadata_load(&out) == 0 && out.seq == 1 && out.active_slot == 0);

    mock_flash_raw(META_ADDR_A)[9] ^= 0x80;                   /* corrupt both */
    CHECK(metadata_load(&out) == -1);
}

static void test_metadata_seq_wrap(void)
{
    puts("metadata seq wrap");
    boot_metadata_t m = mk(0, BOOT_STATE_NORMAL, 0, 0), out;
    mock_flash_reset();
    m.magic = META_MAGIC;
    m.seq = 0xFFFFFFFFu;
    m.crc32 = crc32_calc(&m, offsetof(boot_metadata_t, crc32));
    flash_write(META_ADDR_A, &m, sizeof m);
    m = mk(1, BOOT_STATE_NORMAL, 0, 0);
    CHECK(metadata_store(&m) == 0);
    CHECK(metadata_load(&out) == 0 && out.seq == 0 && out.active_slot == 1);
}

static void seed_prior(int prior)
{
    mock_flash_reset();
    for (int i = 0; i < prior; i++) {
        boot_metadata_t m = mk(i & 1, BOOT_STATE_NORMAL, 0, 1);
        metadata_store(&m);
    }
}

/* Exhaustive power-cut sweep over metadata_store: every op, every byte. */
static void test_metadata_power_cut_sweep(void)
{
    puts("metadata power-cut sweep");
    int points = 0, bad = 0;

    for (int prior = 0; prior < 3; prior++) {       /* 0 = blank, 1/2 = existing state */
        boot_metadata_t m, prev, out;

        seed_prior(prior);
        int before = mock_flash_mutation_count();
        m = mk(1, BOOT_STATE_TRIAL, 1, 7);
        metadata_store(&m);
        int nops = mock_flash_mutation_count() - before;

        for (int op = 0; op < nops; op++) {
            for (size_t partial = 0; partial <= sizeof(boot_metadata_t); partial++) {
                seed_prior(prior);
                memset(&prev, 0, sizeof prev);
                if (prior > 0)
                    metadata_load(&prev);

                mock_flash_arm_cut(mock_flash_mutation_count() + op, partial);
                m = mk(1, BOOT_STATE_TRIAL, 1, 7);
                metadata_store(&m);
                mock_flash_power_on();

                int rc = metadata_load(&out);
                int is_new = rc == 0 && out.active_slot == 1 && out.min_allowed_version == 7;
                int is_old = rc == 0 && out.seq == prev.seq && out.active_slot == prev.active_slot;
                int ok = prior == 0 ? (rc == -1 || is_new) : (is_old || is_new);

                points++;
                if (!ok) {
                    bad++;
                    printf("  cut prior=%d op=%d partial=%zu -> bad\n", prior, op, partial);
                }
            }
        }
    }
    printf("  %d cut points, %d bad\n", points, bad);
    CHECK(bad == 0);
}

static void put_image(uint32_t slot, uint32_t version, size_t size)
{
    static uint8_t payload[4096];
    image_header_t h;
    for (size_t i = 0; i < size; i++)
        payload[i] = (uint8_t)(i * 7 + 3);
    memset(&h, 0, sizeof h);
    h.magic = IMAGE_MAGIC;
    h.version = version;
    h.image_size = (uint32_t)size;
    h.image_crc32 = crc32_calc(payload, size);
    h.header_crc32 = crc32_calc(&h, offsetof(image_header_t, header_crc32));
    flash_write(slot, &h, sizeof h);
    flash_write(slot + IMAGE_HEADER_SIZE, payload, size);
}

static void test_image_basic(void)
{
    puts("image_check_basic");
    image_header_t h, bad;

    mock_flash_reset();
    CHECK(image_check_basic(SLOT_A_ADDR, 0, &h) == IMG_ERR_MAGIC);      /* blank slot */

    put_image(SLOT_A_ADDR, 5, 3000);
    CHECK(image_check_basic(SLOT_A_ADDR, 0, &h) == IMG_OK && h.version == 5);
    CHECK(image_check_basic(SLOT_A_ADDR, 5, &h) == IMG_OK);
    CHECK(image_check_basic(SLOT_A_ADDR, 6, &h) == IMG_ERR_VERSION_FLOOR);

    mock_flash_raw(SLOT_A_ADDR + IMAGE_HEADER_SIZE + 100)[0] ^= 0x10;    /* payload flip */
    CHECK(image_check_basic(SLOT_A_ADDR, 0, &h) == IMG_ERR_IMAGE_CRC);

    mock_flash_reset();
    put_image(SLOT_B_ADDR, 1, 1000);
    mock_flash_raw(SLOT_B_ADDR + 4)[0] ^= 0x01;                          /* header flip */
    CHECK(image_check_basic(SLOT_B_ADDR, 0, &h) == IMG_ERR_HEADER_CRC);

    mock_flash_reset();
    put_image(SLOT_A_ADDR, 1, 100);
    flash_read(SLOT_A_ADDR, &bad, sizeof bad);
    bad.image_size = IMAGE_MAX_SIZE + 1;
    bad.header_crc32 = crc32_calc(&bad, offsetof(image_header_t, header_crc32));
    memcpy(mock_flash_raw(SLOT_A_ADDR), &bad, sizeof bad);
    CHECK(image_check_basic(SLOT_A_ADDR, 0, &h) == IMG_ERR_SIZE);
}

int main(void)
{
    test_crc32();
    test_sha256();
    test_metadata_basic();
    test_metadata_corruption();
    test_metadata_seq_wrap();
    test_metadata_power_cut_sweep();
    test_image_basic();
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
