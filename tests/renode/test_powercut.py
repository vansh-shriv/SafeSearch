#!/usr/bin/env python3
"""Power cuts on the REAL firmware binary under Renode. Run from repo root after `make`:
    python tests/renode/test_powercut.py [--quick]

The running v1 app installs a staged v2 image. A Renode hook on the flash driver's word-program routine
(or sector-erase routine) freezes the CPU at a chosen point, then the machine is reset, exactly as a power
cut would leave flash at that instant (a frozen CPU touches nothing). The bootloader then has to recover.

Cut points: before the target-slot erase; word boundaries inside the payload, the header (written last),
before the metadata sector erase, and at each of the 5 words of the metadata commit. Expected outcome for
every cut before the commit completes: the device boots the old v1. The un-cut control boots v2.

Complements the host sweep (tests/unit/fault_sweep.c), which is exhaustive but runs on the host build;
this runs the compiled ARM binary in the emulator at a sample of the same points.
"""
import os
import re
import struct
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
NM = os.environ.get("SF_NM", r"C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\12.2 mpacbti-rel1\bin\arm-none-eabi-nm.exe")
ELF = "build/app_v1_slotA.elf"

STAGE_ADDR = 0x20020000
STAGE_MAGIC = 0x45475453
SLOT_B = 0x08080000
META_A = 0x08008000
HEADER = 0x400


def path(rel):
    return os.path.join(REPO, rel)


def symbols():
    out = subprocess.run([NM, path(ELF)], capture_output=True, text=True).stdout
    return {m.group(2): int(m.group(1), 16) for m in re.finditer(r"^([0-9a-f]+) [tT] (\S+)$", out, re.M)}


def run(seconds, pre, steps):
    return subprocess.run(
        ["powershell", "-NoProfile", "-File", path("sim/run.ps1"), "-Seconds", str(seconds),
         "-Pre", pre, "-Steps", steps],
        capture_output=True, text=True, cwd=REPO).stdout


def main():
    quick = "--quick" in sys.argv
    sym = symbols()
    for need in ("program_word", "flash_erase_sector", "Default_Handler"):
        if need not in sym:
            print(f"missing symbol {need} in {ELF}; rebuild with `mingw32-make`")
            return 2
    spin = sym["Default_Handler"] | 1   # `for (;;)` in the vector table's default handler; |1 = Thumb

    with open(path("build/app_v2_slotB.img"), "rb") as f:
        img = f.read()
    with open(path("build/stage_pc.bin"), "wb") as f:
        f.write(struct.pack("<II", STAGE_MAGIC, len(img)) + img)
    payload_words = (len(img) - HEADER) // 4

    points = [("erase target slot (sector 8)", "erase", 8)]
    for i in (0, payload_words // 2, payload_words - 1):
        points.append((f"payload word {i}", "word", SLOT_B + HEADER + 4 * i))
    for j in (0, 14, 28, 255):
        points.append((f"header word {j}", "word", SLOT_B + 4 * j))
    points.append(("erase metadata sector (sector 2)", "erase", 2))
    for m in range(5):
        points.append((f"metadata commit word {m}", "word", META_A + 4 * m))
    if quick:
        points = points[:2] + points[4:5] + points[8:10] + points[-2:]

    failed = 0
    stage = "sysbus LoadBinary @build/stage_pc.bin 0x%08X;" % STAGE_ADDR

    # control: no cut, must end on v2
    out = run(3, stage, "machine Reset; emulation RunFor '6';")
    ok = "APP: install ok, resetting" in out and out.rstrip().split("\n")[-1] == "APP: confirmed healthy" and \
        "APP: running, version 2" in out
    print(f"{'PASS' if ok else 'FAIL'}  control (no cut) ends on v2")
    if not ok:
        failed += 1
        print(out)

    for name, kind, value in points:
        fn = sym["program_word"] if kind == "word" else sym["flash_erase_sector"]
        with open(path("build/cut_point.resc"), "w") as f:
            f.write(f'sysbus.cpu AddHook 0x{fn:08X} "if self.GetRegister(0).RawValue == 0x{value:08X}: '
                    f'self.PC = type(self.PC).Create(0x{spin:08X}, 32)"\n')
        out = run(3, stage + " include @build/cut_point.resc;", "machine Reset; emulation RunFor '6';")
        before, sep, after = out.partition("APP: installing update")
        # After the cut (frozen CPU) the machine is reset: the bootloader banner appears again, the old
        # image must come up, and the update must not have been applied.
        recovered = sep and "APP: install ok" not in out and "SafeFlash BL" in after and \
            "BL: no bootable image" not in out and \
            [l for l in out.split("\n") if l.startswith("APP: running")][-1] == "APP: running, version 1"
        print(f"{'PASS' if recovered else 'FAIL'}  cut at {name} -> boots old v1")
        if not recovered:
            failed += 1
            print(out)

    n = len(points) + 1
    print(f"\n{n - failed}/{n} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
