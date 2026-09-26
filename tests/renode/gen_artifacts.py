#!/usr/bin/env python3
"""Generate the test artifacts the Robot suites load, into build/robot_art/. Run from the repo root after `make`.

  attack_*.img        signed-looking images whose CRCs are all self-consistent, so only the crypto can reject them
  stage_*.bin         OTA staging blobs ({'STGE', len, image}) for the RAM staging area
  meta_*.bin          raw boot_metadata_t blobs to stage device states
  vars.py             Robot variables: symbol addresses for the power-cut hooks
"""
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import zlib

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(REPO, "tools"))

from cryptography.hazmat.primitives import serialization  # noqa: E402
from cryptography.hazmat.primitives.asymmetric import ec  # noqa: E402
import mkmeta  # noqa: E402
import sign_image  # noqa: E402

OUT = os.path.join(REPO, "build", "robot_art")
STAGE_MAGIC = 0x45475453
NM = (os.environ.get("SF_NM") or shutil.which("arm-none-eabi-nm") or
      r"C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\12.2 mpacbti-rel1\bin\arm-none-eabi-nm.exe")


def write(name, data):
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(data)


def read(rel):
    with open(os.path.join(REPO, rel), "rb") as f:
        return f.read()


def header(payload, version, key, *, sha=None, crc=None, sig=None):
    """Image with correct CRCs; sha/crc/sig can be forced to stale or attacker values."""
    sha = sha if sha is not None else hashlib.sha256(payload).digest()
    crc = crc if crc is not None else zlib.crc32(payload)
    prefix = struct.pack(sign_image.SIGNED_FMT, sign_image.MAGIC, version, len(payload), crc, sha)
    sig = sig if sig is not None else sign_image.sign_prefix(key, prefix)
    body = prefix + sig
    hdr = body + struct.pack("<I", zlib.crc32(body))
    return hdr.ljust(sign_image.HEADER_SIZE, b"\xff") + payload


def stage(img):
    return struct.pack("<II", STAGE_MAGIC, len(img)) + img


def symbols(elf):
    out = subprocess.run([NM, os.path.join(REPO, elf)], capture_output=True, text=True).stdout
    return {m.group(2): int(m.group(1), 16) for m in re.finditer(r"^([0-9a-f]+) [tT] (\S+)$", out, re.M)}


def main():
    os.makedirs(OUT, exist_ok=True)
    key = serialization.load_pem_private_key(read("keys/private.pem"), password=None)
    v1_img, v2_img = read("build/app_v1_slotA.img"), read("build/app_v2_slotB.img")
    v1_bin, v2_bin = read("build/app_v1_slotA.bin"), read("build/app_v2_slotB.bin")

    # attack images for slot A (payload linked for slot A)
    flipped = bytes([v1_bin[0] ^ 1]) + v1_bin[1:]
    good_sig = v1_img[48:112]
    write("attack_stale_hash.img", header(flipped, 1, key, sha=hashlib.sha256(v1_bin).digest()))
    write("attack_stale_signature.img", header(flipped, 1, key, sig=good_sig))
    write("attack_version_bump.img", header(v1_bin, 9, key, sig=good_sig))
    write("attack_wrong_key.img", header(v1_bin, 1, ec.generate_private_key(ec.SECP256R1())))
    write("attack_zero_signature.img", header(v1_bin, 1, key, sig=bytes(64)))

    # OTA staging blobs
    write("stage_v2.bin", stage(v2_img))
    write("stage_v1.bin", stage(v1_img))
    flipped2 = bytes([v2_bin[0] ^ 1]) + v2_bin[1:]
    write("stage_evil.bin", stage(header(flipped2, 2, key, sig=v2_img[48:112])))

    # device states
    write("meta_trial_b.bin", mkmeta.build(seq=1, active=1, state="trial", trials=0, floor=0))
    write("meta_floor2_trial_a.bin", mkmeta.build(seq=1, active=0, state="trial", trials=0, floor=2))

    sym = symbols("build/app_v1_slotA.elf")
    for need in ("program_word", "flash_erase_sector", "Default_Handler"):
        if need not in sym:
            sys.exit(f"missing symbol {need}; rebuild with mingw32-make")
    with open(os.path.join(OUT, "vars.py"), "w") as f:
        f.write(f"PROGRAM_WORD = '0x{sym['program_word']:08X}'\n")
        f.write(f"FLASH_ERASE = '0x{sym['flash_erase_sector']:08X}'\n")
        f.write(f"SPIN_PC = '0x{sym['Default_Handler'] | 1:08X}'\n")   # for(;;) in Default_Handler; |1 = Thumb
        f.write(f"PAYLOAD_WORDS = {(len(v2_img) - sign_image.HEADER_SIZE) // 4}\n")
    print(f"artifacts written to {OUT}")


if __name__ == "__main__":
    main()
