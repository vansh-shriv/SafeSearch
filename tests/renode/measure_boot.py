#!/usr/bin/env python3
"""Boot cost in EMULATED INSTRUCTIONS (Renode's executed-instruction counter), with vs without signature checks.

    mingw32-make
    mingw32-make BUILD=build_nosig EXTRA_CFLAGS=-DSF_MEASURE_NO_SIGNATURE build_nosig/bootloader.elf
    python tests/renode/measure_boot.py

Instructions, not cycles and not wall-clock: Renode does not model Cortex-M4 pipeline timing, so this is a
relative measure. The no-signature bootloader exists only for this comparison and must never be shipped.
The figure is bootloader entry (`main`) to app entry (`main`) on a steady-state boot (metadata already valid),
i.e. it includes metadata load, structural checks, payload CRC, UART logging, and (normal build) SHA-256 +
ECDSA P-256 verification of one image.
"""
import os
import re
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
RENODE = r"C:\Program Files\Renode\bin\Renode.exe"
NM = os.environ.get("SF_NM", r"C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\12.2 mpacbti-rel1\bin\arm-none-eabi-nm.exe")


def main_addr(elf):
    out = subprocess.run([NM, os.path.join(REPO, elf)], capture_output=True, text=True).stdout
    return int(re.search(r"^([0-9a-f]+) T main$", out, re.M).group(1), 16)


def measure(bl_elf):
    bl_main = main_addr(bl_elf)
    app_main = main_addr("build/app_v1_slotA.elf")
    hooks = os.path.join(REPO, "build", "measure.resc")
    with open(hooks, "w") as f:
        f.write(f'sysbus.cpu AddHook 0x{bl_main:08X} "print \'BL_MAIN\', self.ExecutedInstructions"\n')
        f.write(f'sysbus.cpu AddHook 0x{app_main:08X} "print \'APP_MAIN\', self.ExecutedInstructions"\n')
    cmd = (f"$bl=@{bl_elf}; include @sim/boot.resc; include @build/measure.resc; "
           "emulation RunFor '4'; machine Reset; emulation RunFor '4'; quit")
    out = subprocess.run([RENODE, "--console", "--disable-xwt", "--plain", "-e", cmd],
                         capture_output=True, text=True, cwd=REPO).stdout
    bl = re.findall(r"BL_MAIN (\d+)", out)
    app = re.findall(r"APP_MAIN (\d+)", out)
    if len(bl) < 2 or len(app) < 2:
        print(f"could not parse two boots for {bl_elf}: BL_MAIN={bl} APP_MAIN={app}")
        sys.exit(2)
    # The counter restarts at every machine reset; second boot = steady state (metadata already valid).
    return int(app[1]) - int(bl[1])


def main():
    with_sig = measure("build/bootloader.elf")
    without = measure("build_nosig/bootloader.elf")
    print(f"boot (BL entry -> app entry), emulated instructions, steady state:")
    print(f"  with SHA-256 + ECDSA verify : {with_sig:>12,}")
    print(f"  without signature check     : {without:>12,}")
    print(f"  signature verify cost       : {with_sig - without:>12,}  ({(with_sig - without) / with_sig:.1%} of boot)")


if __name__ == "__main__":
    main()
