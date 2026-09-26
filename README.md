# SafeFlash

A secure bootloader with signed A/B over-the-air updates for an ARM Cortex-M (STM32F4), built and tested
**entirely without hardware**: firmware runs under [Renode](https://renode.io), and power-loss behaviour is
tested exhaustively on the host.

What it guarantees:

1. No image runs unless its ECDSA P-256 signature (and payload SHA-256) verifies against the baked-in key.
2. An update never touches the running firmware; it lands in the inactive slot.
3. A newly installed image gets a bounded trial period; if it does not confirm itself, the watchdog resets
   the device and, after 3 attempts, the bootloader reverts to the previous image.
4. Power loss at any point during an update, confirm or boot metadata write leaves the device bootable.
5. An older signed image cannot be forced back once a newer one has been confirmed (anti-rollback ratchet).

**Scope, stated plainly:** simulator only. Current/power draw was not measured and no number is claimed. OTA
transport is simulated (the host stages a signed image in RAM). See `docs/design.md`, section "Simulator-only
scope".

## Results

2,803 distinct power-cut points across the OTA install, confirm and bootloader metadata paths, 0 violations.
The harness was validated by injecting real defects, which it caught (2,705 and 88 violations respectively).
Renode suites: signature attacks 6/6, trial and revert 12/12, OTA and anti-rollback 8/8. Bootloader 6.9 KB
of a 32 KB region. Details in `docs/results.md`.

## Layout

```
bootloader/   boot logic (hardware-independent), STM32 flash driver, linker script
app/          demo app + SafeFlash app library (confirm, watchdog gating, installer)
crypto/       own SHA-256, vendored micro-ecc (P-256)
tools/        keytool.py, sign_image.py, mkmeta.py, summarize_sweep.py
sim/          Renode platform script and headless runner
tests/unit/   host unit tests + exhaustive power-cut sweep
tests/renode/ Renode suites (signature, trial, OTA)
docs/         design.md, results.md
```

## Quick start (Windows)

Needs: Renode, Arm GNU Toolchain (`arm-none-eabi-gcc`), MinGW `mingw32-make`, Python 3 with `cryptography`.
None of them are on PATH by default here; see `PROGRESS.md` for the paths used.

```
mingw32-make                            # builds firmware, generates a dev key in keys/ (gitignored)
mingw32-make -C tests/unit test         # host unit tests
mingw32-make -C tests/unit sweep        # exhaustive power-cut sweep -> build/fault_sweep.csv
python tools/summarize_sweep.py         # results table
powershell -NoProfile -File sim/run.ps1 -Seconds 2   # boot in Renode, print UART
python tests/renode/test_signature.py
python tests/renode/test_trial.py
python tests/renode/test_ota.py
```

The dev private key is generated locally and must never be committed. `keys/` is in `.gitignore`.

## Status

Phases 0-5 complete. Phase 6 partly done; remaining work is tracked in `PROGRESS.md`.
