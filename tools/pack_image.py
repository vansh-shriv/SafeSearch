#!/usr/bin/env python3
"""Wrap a raw app .bin in a SafeFlash image header (CRC only; sha256/signature zeroed).

Phase 1 stand-in. Phase 3 replaces this with sign_image.py, which fills sha256 + ECDSA signature.
Layout must match bootloader/src/image_verify.h and flash_map.h.
"""
import argparse
import struct
import zlib

MAGIC = 0x53414645
HEADER_SIZE = 0x400
HDR_FMT = "<IIII32s64s"  # header_crc32 appended after


def build(payload: bytes, version: int) -> bytes:
    body = struct.pack(HDR_FMT, MAGIC, version, len(payload), zlib.crc32(payload), bytes(32), bytes(64))
    hdr = body + struct.pack("<I", zlib.crc32(body))
    return hdr.ljust(HEADER_SIZE, b"\xff") + payload


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bin")
    ap.add_argument("out")
    ap.add_argument("--version", type=int, required=True)
    a = ap.parse_args()
    with open(a.bin, "rb") as f:
        payload = f.read()
    with open(a.out, "wb") as f:
        f.write(build(payload, a.version))


if __name__ == "__main__":
    main()
