#!/usr/bin/env python3
"""Build a raw boot_metadata_t blob (test/staging helper). Layout matches bootloader/src/metadata.h.

Load into metadata copy A at 0x08008000 (or B at 0x0800C000) before boot, e.g. via Renode LoadBinary.
"""
import argparse
import struct
import zlib

META_MAGIC = 0x4D455441
STATES = {"normal": 0, "trial": 1, "confirmed": 2, "revert_pending": 3}


def build(seq, active, state, trials, floor):
    body = struct.pack("<IIBBBBI", META_MAGIC, seq, active, STATES[state], trials, 0, floor)
    return body + struct.pack("<I", zlib.crc32(body))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--seq", type=int, default=1)
    ap.add_argument("--active", type=int, choices=(0, 1), required=True)
    ap.add_argument("--state", choices=STATES, required=True)
    ap.add_argument("--trials", type=int, default=0)
    ap.add_argument("--floor", type=int, default=0)
    a = ap.parse_args()
    with open(a.out, "wb") as f:
        f.write(build(a.seq, a.active, a.state, a.trials, a.floor))


if __name__ == "__main__":
    main()
