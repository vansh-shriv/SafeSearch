#!/usr/bin/env python3
"""Wrap a raw app .bin in a signed SafeFlash image.

Header layout (must match bootloader/src/image_verify.h, flash_map.h, image_crypto.h):
  magic u32 | version u32 | image_size u32 | image_crc32 u32 | sha256[32] | signature[64] | header_crc32 u32
padded with 0xFF to 0x400, followed by the payload.
The signature is ECDSA P-256 over SHA-256(first 48 header bytes), raw r||s.
"""
import argparse
import hashlib
import os
import struct
import zlib

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature

MAGIC = 0x53414645
HEADER_SIZE = 0x400
SIGNED_FMT = "<IIII32s"  # the 48 signed bytes


def sign_prefix(key, prefix: bytes) -> bytes:
    der = key.sign(prefix, ec.ECDSA(hashes.SHA256()))
    r, s = decode_dss_signature(der)
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def build(payload: bytes, version: int, key) -> bytes:
    prefix = struct.pack(SIGNED_FMT, MAGIC, version, len(payload), zlib.crc32(payload),
                         hashlib.sha256(payload).digest())
    body = prefix + sign_prefix(key, prefix)
    hdr = body + struct.pack("<I", zlib.crc32(body))
    return hdr.ljust(HEADER_SIZE, b"\xff") + payload


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bin")
    ap.add_argument("out")
    ap.add_argument("--version", type=int, required=True)
    ap.add_argument("--key", default=os.path.join("keys", "private.pem"))
    a = ap.parse_args()
    with open(a.key, "rb") as f:
        key = serialization.load_pem_private_key(f.read(), password=None)
    with open(a.bin, "rb") as f:
        payload = f.read()
    with open(a.out, "wb") as f:
        f.write(build(payload, a.version, key))


if __name__ == "__main__":
    main()
