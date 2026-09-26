#!/usr/bin/env python3
"""Attack images vs the bootloader under Renode.

Every attack keeps all *unkeyed* fields (CRCs) self-consistent, so only the SHA-256 / ECDSA checks can
reject it. Slot A gets the attack image; Slot B holds the genuine v2, so a correct bootloader must report
the expected reason for slot A and fall back to B. Run from the repo root:  python tests/renode/test_signature.py
"""
import hashlib
import os
import struct
import subprocess
import sys
import zlib

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(REPO, "tools"))

from cryptography.hazmat.primitives import serialization  # noqa: E402
from cryptography.hazmat.primitives.asymmetric import ec  # noqa: E402
import sign_image  # noqa: E402

BUILD = os.path.join(REPO, "build")


def header(payload, version, key, *, sha=None, crc=None, sig=None):
    """Header with correct CRCs. sha/crc/sig can be forced to stale/attacker values."""
    sha = sha if sha is not None else hashlib.sha256(payload).digest()
    crc = crc if crc is not None else zlib.crc32(payload)
    prefix = struct.pack(sign_image.SIGNED_FMT, sign_image.MAGIC, version, len(payload), crc, sha)
    sig = sig if sig is not None else sign_image.sign_prefix(key, prefix)
    body = prefix + sig
    hdr = body + struct.pack("<I", zlib.crc32(body))
    return hdr.ljust(sign_image.HEADER_SIZE, b"\xff") + payload


def main():
    with open(os.path.join(REPO, "keys", "private.pem"), "rb") as f:
        key = serialization.load_pem_private_key(f.read(), password=None)
    with open(os.path.join(BUILD, "app_v1_slotA.bin"), "rb") as f:
        payload = f.read()
    flipped = bytes([payload[0] ^ 1]) + payload[1:]
    good_img = header(payload, 1, key)
    good_prefix = good_img[:48]
    good_sig = good_img[48:112]

    cases = {
        # name: (image bytes, expected substring for slot A line)
        "control_valid": (good_img, "slot A: ok"),
        # payload changed, CRCs recomputed, but the signed sha256 is stale
        "stale_hash": (header(flipped, 1, key, sha=hashlib.sha256(payload).digest()), "slot A: bad payload hash"),
        # payload + sha256 + CRCs all consistent, signature is from the original
        "stale_signature": (header(flipped, 1, key, sig=good_sig), "slot A: bad signature"),
        # version bumped (e.g. to dodge a floor), everything else consistent, old signature
        "version_bump": (header(payload, 9, key, sig=good_sig), "slot A: bad signature"),
        # attacker signs with their own key
        "wrong_key": (header(payload, 1, ec.generate_private_key(ec.SECP256R1())), "slot A: bad signature"),
        "zero_signature": (header(payload, 1, key, sig=bytes(64)), "slot A: bad signature"),
    }

    failed = 0
    for name, (img, expect) in cases.items():
        rel = f"build/attack_{name}.img"
        with open(os.path.join(REPO, rel), "wb") as f:
            f.write(img)
        out = subprocess.run(
            ["powershell", "-NoProfile", "-File", os.path.join(REPO, "sim", "run.ps1"),
             "-Seconds", "3", "-SlotA", rel],
            capture_output=True, text=True, cwd=REPO).stdout
        ok = expect in out
        # rejected attack must fall back to genuine v2; the control must boot v1
        want_ver = "version 1" if name == "control_valid" else "version 2"
        ok = ok and f"APP: running, {want_ver}" in out
        print(f"{'PASS' if ok else 'FAIL'}  {name:16s} expect '{expect}'")
        if not ok:
            failed += 1
            print(out)
    print(f"\n{len(cases) - failed}/{len(cases)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
