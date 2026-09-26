#!/usr/bin/env python3
"""Seed corpora for the libFuzzer binaries: tests/fuzz/corpus/<target>/. Run from the repo root after `make`.

libFuzzer has no notion of the structured inputs these targets expect, so it starts from valid ones:
genuine images (with the fixup flag bytes), a valid recovery transfer stream, and small synthetic blobs.
"""
import os
import random
import struct
import sys
import zlib

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(REPO, "tools"))
import recover  # noqa: E402  (frame builder shared with the real host tool)

OUT = os.path.join(REPO, "tests", "fuzz", "corpus")


def read(rel):
    with open(os.path.join(REPO, rel), "rb") as f:
        return f.read()


def write(target, name, data):
    d = os.path.join(OUT, target)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name), "wb") as f:
        f.write(data)


def stream(img):
    out = recover.frame(recover.BEGIN, struct.pack("<I", len(img)))
    for off in range(0, len(img), recover.CHUNK):
        out += recover.frame(recover.DATA, struct.pack("<I", off) + img[off:off + recover.CHUNK])
    return out + recover.frame(recover.END)


def main():
    imgs = [read("build/app_v1_slotA.img"), read("build/app_v2_slotB.img")]
    rng = random.Random(1)
    for i, img in enumerate(imgs):
        for flags in (0, 1, 3, 7):
            write("fuzz_image", f"img{i}_f{flags}", bytes([flags | (i << 3)]) + img)
        for flags in (0, 7, 0x10, 0x20, 0x30):   # bits 4-5 select the anti-rollback floor 0..3
            write("fuzz_install", f"img{i}_f{flags}", bytes([flags]) + img)
    for i in range(6):
        write("fuzz_metadata", f"m{i}", bytes([i & 3]) + rng.randbytes(40))
    for mode in (0x00, 0x05, 0x25, 0x35, 0x2B, 0x6F, 0x3F, 0x25 | 0x10):
        write("fuzz_boot", f"b{mode:02x}", bytes([mode]) + rng.randbytes(24))
    for healthy in (0, 1):
        for fix in (0, 2):
            write("fuzz_recovery", f"r{healthy}{fix}", bytes([healthy | fix]) + stream(imgs[1]))
    print(f"corpora written to {OUT}")


if __name__ == "__main__":
    main()
