/*
 * Portable mutation fuzzer for the targets in fuzz_targets.c. Not coverage-guided like libFuzzer; it mutates
 * the genuine seed images (bit flips, interesting values, splices, truncation, growth) and keeps any input that
 * reaches a new outcome "feature" as feedback. Deterministic for a given seed, so failures replay.
 *
 *   fuzz_driver <image|metadata|boot|install|recovery> [iterations] [rng-seed]
 *   fuzz_driver replay <target> <file>
 */
#include "fuzz_targets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LEN   (16 * 1024)
#define MAX_CORP  512

static uint64_t rng_state;

static uint32_t rnd(void)
{
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return (uint32_t)(x >> 16);
}

static const uint8_t interesting[] = { 0x00, 0x01, 0x7F, 0x80, 0xFF, 0x55, 0xAA };

typedef int (*target_fn)(const uint8_t *, size_t);

static struct { uint8_t *data; size_t len; } corpus[MAX_CORP];
static size_t ncorp;

static void add_corpus(const uint8_t *d, size_t n)
{
    if (ncorp >= MAX_CORP)
        return;
    corpus[ncorp].data = malloc(n ? n : 1);
    memcpy(corpus[ncorp].data, d, n);
    corpus[ncorp].len = n;
    ncorp++;
}

static size_t mutate(uint8_t *buf, size_t len)
{
    unsigned rounds = 1 + rnd() % 4;

    while (rounds--) {
        switch (rnd() % 9) {
        case 0:  if (len) buf[rnd() % len] ^= (uint8_t)(1u << (rnd() & 7)); break;
        case 1:  if (len) buf[rnd() % len] = (uint8_t)rnd(); break;
        case 2:  if (len) buf[rnd() % len] = interesting[rnd() % sizeof interesting]; break;
        case 3:  /* overwrite an aligned-ish word: sizes, versions, magics */
            if (len >= 4) {
                size_t o = (rnd() % (len - 3)) & ~3u;
                uint32_t v = (rnd() & 1) ? (uint32_t)rnd() : (uint32_t)(1u << (rnd() % 32));
                memcpy(buf + o, &v, 4);
            }
            break;
        case 4:  /* header-focused: first 128 bytes are where the structure lives */
            if (len) buf[rnd() % (len < 128 ? len : 128)] ^= (uint8_t)(1u << (rnd() & 7));
            break;
        case 5:  if (len > 8) len -= rnd() % (len < 512 ? len / 2 : 256); break;   /* truncate */
        case 6:  { size_t add = 1 + rnd() % 64; if (len + add <= MAX_LEN) { memset(buf + len, (int)(rnd() & 0xFF), add); len += add; } break; }
        case 7:  /* splice from another corpus entry */
            if (ncorp && len) {
                size_t k = rnd() % ncorp, n = 1 + rnd() % 64;
                if (n > corpus[k].len) n = corpus[k].len;
                if (n && corpus[k].len >= n) {
                    size_t so = rnd() % (corpus[k].len - n + 1), dofs = rnd() % (len > n ? len - n + 1 : 1);
                    memcpy(buf + dofs, corpus[k].data + so, n);
                }
            }
            break;
        default: if (len) buf[0] = (uint8_t)rnd(); break;   /* flags byte */
        }
    }
    return len;
}

static target_fn pick(const char *name)
{
    if (!strcmp(name, "image")) return fuzz_image;
    if (!strcmp(name, "metadata")) return fuzz_metadata;
    if (!strcmp(name, "boot")) return fuzz_boot;
    if (!strcmp(name, "install")) return fuzz_install;
    if (!strcmp(name, "recovery")) return fuzz_recovery;
    return NULL;
}

int main(int argc, char **argv)
{
    static uint8_t buf[MAX_LEN + 64];
    static uint8_t seen[1024];
    unsigned long iters = 20000;
    unsigned long i, novel = 0;
    target_fn fn;

    if (argc < 2 || fuzz_init())
        return argc < 2 ? (fprintf(stderr, "usage: fuzz_driver <image|metadata|boot|install|recovery> [iters] [seed] | replay <target> <file>\n"), 2) : 2;

    if (!strcmp(argv[1], "replay") && argc >= 4) {
        FILE *f = fopen(argv[3], "rb");
        size_t n;
        if (!(fn = pick(argv[2])) || !f)
            return 2;
        n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        fn(buf, n);
        printf("replay ok\n");
        return 0;
    }

    if (!(fn = pick(argv[1])))
        return 2;
    if (argc > 2) iters = strtoul(argv[2], NULL, 10);
    rng_state = argc > 3 ? strtoull(argv[3], NULL, 10) | 1 : 0x9E3779B97F4A7C15ull;

    /* Seeds: each genuine image with a spread of flag bytes, plus small synthetic inputs for metadata/boot. */
    if (!strcmp(argv[1], "recovery")) {   /* seeds are complete, valid transfer streams */
        for (size_t s = 0; s < fuzz_recovery_seed_count(); s++) {
            size_t len;
            const uint8_t *st = fuzz_recovery_seed(s, &len);
            for (unsigned flags = 0; flags < 4; flags += 2) {
                memcpy(buf, st, len);
                buf[0] = (uint8_t)((st[0] & 1) | flags);
                add_corpus(buf, len);
            }
        }
    }
    for (size_t s = 0; s < fuzz_seed_count(); s++) {
        size_t len;
        const uint8_t *img = fuzz_seed(s, &len);
        if (!strcmp(argv[1], "recovery"))
            break;
        for (unsigned flags = 0; flags < 8; flags += 3) {
            buf[0] = (uint8_t)(flags | (s << 3));
            memcpy(buf + 1, img, len);
            add_corpus(buf, len + 1);
        }
    }
    for (int k = 0; k < 8; k++) {
        size_t n = 64 + k * 16;
        for (size_t j = 0; j < n; j++) buf[j] = (uint8_t)rnd();
        add_corpus(buf, n);
    }

    for (i = 0; i < iters; i++) {
        size_t k = rnd() % ncorp, len = corpus[k].len;
        uint32_t f;
        memcpy(buf, corpus[k].data, len);
        len = mutate(buf, len);
        fn(buf, len);
        f = fuzz_last_feature() % (sizeof seen * 8);
        if (!(seen[f / 8] & (1u << (f % 8)))) {
            seen[f / 8] |= (uint8_t)(1u << (f % 8));
            add_corpus(buf, len);
            novel++;
        }
    }
    {
        unsigned features = 0;
        for (size_t b = 0; b < sizeof seen; b++)
            for (int bit = 0; bit < 8; bit++)
                features += (seen[b] >> bit) & 1;
        printf("%-9s %lu iterations, %u distinct outcomes reached, corpus %zu, no property violations\n",
               argv[1], iters, features, ncorp);
    }
    return 0;
}
