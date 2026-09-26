#!/usr/bin/env python3
"""Trial boot / confirm / watchdog revert under Renode. Run from repo root after `make`:
    python tests/renode/test_trial.py

Staging: metadata for "slot B holds a freshly installed image in TRIAL, slot A is the confirmed old image"
is loaded straight into metadata copy A (0x08008000) before the machine starts (Phase 5 will produce this
state through the real update path).
"""
import os
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(REPO, "tools"))
import mkmeta  # noqa: E402

META_A = 0x08008000


def run(slot_b, seconds, meta, steps=""):
    rel = "build/meta_test.bin"
    with open(os.path.join(REPO, rel), "wb") as f:
        f.write(meta)
    pre = f"sysbus LoadBinary @{rel} 0x{META_A:08X};"
    return subprocess.run(
        ["powershell", "-NoProfile", "-File", os.path.join(REPO, "sim", "run.ps1"),
         "-Seconds", str(seconds), "-SlotB", slot_b, "-Pre", pre, "-Steps", steps],
        capture_output=True, text=True, cwd=REPO).stdout


failures = 0


def check(name, cond, out):
    global failures
    print(f"{'PASS' if cond else 'FAIL'}  {name}")
    if not cond:
        failures += 1
        print("---- UART ----\n" + out + "--------------")


def main():
    trial_b = mkmeta.build(seq=1, active=1, state="trial", trials=0, floor=0)

    # 1. good image confirms itself: one boot, no watchdog reset even well past the trial window, then normal boots
    out = run("build/app_v2_slotB.img", 6, trial_b,
              steps="machine Reset; sysbus.cpu VectorTableOffset 0x08000000; emulation RunFor '4';")
    boots = out.count("SafeFlash BL")
    check("good v2: trial armed on first boot", "BL: trial 1/3, watchdog armed" in out, out)
    check("good v2: confirms itself", "APP: confirmed healthy" in out, out)
    check("good v2: no watchdog reset while running confirmed (2 boots total = 1 trial + 1 manual reset)",
          boots == 2, out)
    check("good v2: after confirm the next boot is not a trial",
          out.count("watchdog armed") == 1 and "state 2 trials 0" in out, out)

    # 2. bad v3 never confirms: watchdog resets it MAX_TRIALS times, then bootloader reverts to slot A (v1)
    out = run("build/app_v3_slotB_bad.img", 16, trial_b)
    for n in (1, 2, 3):
        check(f"bad v3: trial {n}/3 runs", f"BL: trial {n}/3, watchdog armed" in out, out)
    check("bad v3: never confirms", "APP: confirmed healthy" not in out.split("BL: trial limit reached")[0], out)
    check("bad v3: trial limit reached, reverted to slot A", "BL: trial limit reached" in out and
          "BL: reverted to slot A" in out, out)
    check("bad v3: old v1 running after revert", out.rstrip().endswith("APP: confirmed healthy") and
          out.count("APP: running, version 1") == 1, out)
    check("bad v3: exactly MAX_TRIALS+1 boots (3 trials + 1 revert boot)", out.count("SafeFlash BL") == 4, out)
    check("bad v3: stays on v1 after revert (no more resets)", out.strip().split("\n")[-1] == "APP: confirmed healthy", out)

    print(f"\n{'ALL PASSED' if not failures else str(failures) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
