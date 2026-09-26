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
6. A device with nothing bootable does not brick: it enters a serial recovery mode that accepts only a verified,
   signed image.

**Scope, stated plainly:** simulator only. Current/power draw was not measured and no number is claimed. OTA
transport is simulated (the host stages a signed image in RAM). See `docs/design.md`, section "Simulator-only
scope".

## Results

- **Power-cut sweep:** 5,526 distinct fault points across OTA install, confirm, bootloader metadata writes and
  serial recovery, 0 violations, with real crypto.
- **Model check:** every reachable device state (10,750) under every interleaving of boot, confirm, install and
  tampering with power cuts, 658,946 transitions, 7 properties hold.
- **Fuzzing:** 5 property-checking targets (images, metadata, whole-device state, install, recovery stream),
  no violations. A libFuzzer + ASan/UBSan job is defined in CI (its first run is still pending).
- **Validated by injection:** each of the sweep, fuzzers, model checker and recovery tests was shown to fail
  when real defects were injected into the production code.
- **Emulator:** 32 Robot Framework tests on the compiled ARM binary, including power cuts at 14 chosen
  points and serial recovery over a socket.
- **Size and cost:** bootloader 8.0 KB of a 32 KB region; verifying a signature is 98.8% of boot cost
  (7.92 M emulated instructions, not cycles).

Details in `docs/results.md`.

## Layout

```
bootloader/   boot logic (hardware-independent), STM32 flash driver, linker script
app/          demo app + SafeFlash app library (confirm, watchdog gating, installer)
crypto/       own SHA-256, vendored micro-ecc (P-256)
tools/        keytool.py, sign_image.py, recover.py (serial recovery host), mkmeta.py, summarize_sweep.py
sim/          Renode platform script, ad-hoc runner, Robot Framework suites (sim/robot/)
tests/unit/   host unit tests, recovery tests, exhaustive power-cut sweep
tests/fuzz/   property-checking fuzz targets (portable driver + libFuzzer)
tests/model/  explicit-state model checker and its mutation test
tests/renode/ Robot test-artifact generator, boot-cost measurement
docs/         design.md, results.md
```

## Quick start (Windows)

Needs: Renode, Arm GNU Toolchain (`arm-none-eabi-gcc`), MinGW `mingw32-make`, Python 3 with `cryptography`.
None of them are on PATH by default here; see `PROGRESS.md` for the paths used.

```
mingw32-make                            # builds firmware, generates a dev key in keys/ (gitignored)
mingw32-make -C tests/unit test         # host unit tests
mingw32-make -C tests/unit recovery     # serial recovery tests with real signatures
mingw32-make -C tests/unit sweep        # exhaustive power-cut sweep -> build/fault_sweep.csv
mingw32-make -C tests/model check       # model check of all reachable device states
mingw32-make -C tests/fuzz run ITERS=50000   # fuzz all five targets
python tools/summarize_sweep.py         # results table
powershell -NoProfile -File sim/run.ps1 -Seconds 2   # boot in Renode, print UART
powershell -NoProfile -File sim/run_robot.ps1        # Robot suites: signature, trial, ota, powercut, recovery (-Suite ota,recovery to pick)
python tests/renode/measure_boot.py     # needs the build_nosig bootloader, see the script header
```

The dev private key is generated locally and must never be committed. `keys/` is in `.gitignore`.

## Status

Phases 0-6 complete, plus recovery mode, fuzzing and model checking. See `PROGRESS.md`.
