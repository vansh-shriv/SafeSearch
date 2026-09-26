#!/usr/bin/env python3
"""Simulated OTA + anti-rollback under Renode. Run from repo root after `make`:
    python tests/renode/test_ota.py

Slot A holds the factory v1 (metadata auto-initialises to active A). "OTA delivery" drops a signed image
into the RAM staging area (see app/src/main.c); the running app installs it into the inactive slot.
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
import mkmeta  # noqa: E402
import sign_image  # noqa: E402

STAGE_ADDR = 0x20020000
STAGE_MAGIC = 0x45475453
META_A = 0x08008000
RESET = "machine Reset; "


def path(rel):
    return os.path.join(REPO, rel)


def stage_blob(img, rel):
    with open(path(rel), "wb") as f:
        f.write(struct.pack("<II", STAGE_MAGIC, len(img)) + img)
    return f"sysbus LoadBinary @{rel} 0x{STAGE_ADDR:08X};"


def run(seconds, pre="", steps="", slot_a=None, slot_b=None):
    cmd = ["powershell", "-NoProfile", "-File", path("sim/run.ps1"), "-Seconds", str(seconds),
           "-Pre", pre, "-Steps", steps]
    if slot_a:
        cmd += ["-SlotA", slot_a]
    if slot_b:
        cmd += ["-SlotB", slot_b]
    return subprocess.run(cmd, capture_output=True, text=True, cwd=REPO).stdout


failures = 0


def check(name, cond, out):
    global failures
    print(f"{'PASS' if cond else 'FAIL'}  {name}")
    if not cond:
        failures += 1
        print("---- UART ----\n" + out + "--------------")


def read(rel):
    with open(path(rel), "rb") as f:
        return f.read()


def main():
    with open(path("keys/private.pem"), "rb") as f:
        key = serialization.load_pem_private_key(f.read(), password=None)
    v2 = read("build/app_v2_slotB.img")
    v1 = read("build/app_v1_slotA.img")

    # 1. OTA v1 -> v2, then the ratchet: v2 confirms and raises the floor to 2
    out = run(9, pre=stage_blob(v2, "build/stage_v2.bin"),
              steps=RESET + "emulation RunFor '5';")
    check("ota: app installs staged update", "APP: installing update" in out and "APP: install ok, resetting" in out, out)
    check("ota: reset boots new image in trial", "active B state 1 trials 0" in out and
          "BL: trial 1/3, watchdog armed" in out, out)
    check("ota: new image runs and confirms", "APP: running, version 2" in out and
          out.count("APP: confirmed healthy") >= 2, out)
    check("ota: after confirm, floor ratcheted to 2 and state confirmed",
          "state 2 trials 0 floor 0x00000002" in out, out)

    # 2. rollback attempt: valid signed v1 offered while v2 (floor 2) is confirmed
    out = run(9, pre=stage_blob(v2, "build/stage_v2.bin"),
              steps=RESET + "emulation RunFor '5'; machine Reset; " +
              stage_blob(v1, "build/stage_v1.bin") + " emulation RunFor '3';")
    check("anti-rollback: installer refuses older signed image (rc -3)",
          "APP: install failed rc 0xFFFFFFFD" in out, out)
    before, _, after = out.partition("APP: install failed rc 0xFFFFFFFD")
    check("anti-rollback: refused on v2, no reboot into anything else afterwards",
          "APP: running, version 2" in before.rsplit("SafeFlash BL", 1)[-1] and "SafeFlash BL" not in after, out)

    # 3. bootloader enforces the floor even if metadata points at an old image (A=v1, floor 2, B=v2)
    meta = mkmeta.build(seq=1, active=0, state="trial", trials=0, floor=2)
    with open(path("build/meta_floor.bin"), "wb") as f:
        f.write(meta)
    out = run(6, pre=f"sysbus LoadBinary @build/meta_floor.bin 0x{META_A:08X};", slot_b="build/app_v2_slotB.img")
    check("floor: bootloader rejects v1 below floor and boots v2",
          "BL: slot A: below version floor" in out and "APP: running, version 2" in out, out)

    # 4. a delivered image with a bad signature is installed (installer does not verify) but the
    #    bootloader rejects it and reverts to the old image
    with open(path("build/app_v2_slotB.bin"), "rb") as f:
        payload = f.read()
    flipped = bytes([payload[0] ^ 1]) + payload[1:]
    good_sig = v2[48:112]
    prefix = struct.pack(sign_image.SIGNED_FMT, sign_image.MAGIC, 2, len(flipped), zlib.crc32(flipped),
                         hashlib.sha256(flipped).digest())
    body = prefix + good_sig
    evil = (body + struct.pack("<I", zlib.crc32(body))).ljust(sign_image.HEADER_SIZE, b"\xff") + flipped
    out = run(10, pre=stage_blob(evil, "build/stage_evil.bin"))
    check("evil ota: bootloader rejects bad signature and reverts to v1",
          "BL: slot B: bad signature" in out and "BL: reverted to slot A" in out and
          out.rstrip().endswith("APP: confirmed healthy") and out.count("APP: running, version 1") == 2, out)

    print(f"\n{'ALL PASSED' if not failures else str(failures) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
