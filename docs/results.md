# Results

All numbers below were measured in this repository. Regenerate them with the commands in `README.md`.
Nothing here is a hardware measurement. Current/power draw was not measured: this project targets logic and
fail-safety, not power characterisation.

## Exhaustive power-cut sweep (host, real code, real crypto)

`mingw32-make -C tests/unit sweep` (about 12 s). Production boot, install, confirm and metadata code, with real
SHA-256 and ECDSA P-256 verification, runs against a mock flash that is cut at every flash mutation and at every
byte boundary inside each write, with three torn-erase states per erase. After each cut the device boots, and
the harness asserts:

- it boots some signature-verified image (never bricked);
- that image is the old or the new one, nothing else;
- the boot metadata is valid afterwards;
- the anti-rollback floor never decreases and never exceeds the running version;
- the system converges: an interrupted update either ends in a confirmed image or reverts.

For the OTA install case, the boot after the cut must also be stable once the app confirms. Every (cut point,
outcome) pair is written to `build/fault_sweep.csv`.

| Scenario | Flash mutations cut | Distinct fault points | Recovered correctly | Ended on v1 / v2 |
|---|---:|---:|---:|---|
| `ota_install` | 5 | 2707 | 2707 | 2706 / 1 |
| `confirm` | 2 | 24 | 24 | 0 / 24 |
| `boot_first_boot` | 2 | 24 | 24 | 24 / 0 |
| `boot_trial_increment` | 2 | 24 | 24 | 0 / 24 |
| `boot_revert` | 2 | 24 | 24 | 24 / 0 |
| **Total** | | **2803** | **2803** | |

The single v2 outcome in `ota_install` is the un-cut control run. Every cut before the metadata commit
completes leaves the device on the old v1 image, which is the atomic behaviour intended.

### The harness was itself tested

A sweep that reports zero violations proves little unless it can fail. Injecting real defects:

| Injected defect | Violations found |
|---|---:|
| Installer erases the *active* slot instead of the inactive one | 2705 of 2707 points (device bricked or ratchet wrong) |
| Metadata rewritten in place instead of ping-pong | 88 (anti-rollback floor decreased) |

Deliberately committing metadata before writing the image is not a violation: the bootloader's fallback
reverts a torn image, so that ordering is not safety-critical. The installer keeps the safe order anyway.

## Host unit tests

`mingw32-make -C tests/unit test`: 26 checks pass. They cover CRC-32, SHA-256 known-answer vectors (including
one million `a`), metadata ping-pong, corruption fallback, sequence wrap, header validation, and a
126-point power-cut sweep of the metadata write.

## Renode tests (emulated STM32F4, real firmware binaries)

| Suite | Result | What it shows |
|---|---|---|
| `tests/renode/test_signature.py` | 6/6 | Valid image boots. Attack images with self-consistent CRCs (stale hash, stale signature, version bump, wrong key, zeroed signature) are each rejected for the expected reason and the device falls back to the genuine image |
| `tests/renode/test_trial.py` | 12/12 | A good image confirms and survives past the watchdog window and a reset. A "bad" image that never confirms gets exactly 3 watchdog-reset trials, then the bootloader reverts to the previous slot and stays there |
| `tests/renode/test_powercut.py` | 15/15 | Power cuts on the real ARM binary: a Renode hook on the flash driver freezes the CPU at a chosen point during an OTA install, the machine is reset, and the bootloader must recover. Points: before the target-slot erase, three payload word boundaries, four header word boundaries, before the metadata sector erase, and each of the 5 words of the metadata commit. Every cut boots the old v1; the un-cut control boots v2 |
| `tests/renode/test_ota.py` | 8/8 | OTA v1 to v2 installs, boots in trial, confirms, and raises the floor to 2. The installer refuses a validly signed v1 below the floor. The bootloader refuses v1 below the floor even if metadata points at it. A bad-signature image is installed but rejected at boot and reverted |

## Sizes (arm-none-eabi-size, `-Os`)

| Binary | text bytes |
|---|---:|
| Bootloader (of a 32 KB region) | 6860 |
| Demo app v1 / v2 | 1656 |
| Demo app v3 (never confirms) | 1452 |

Signed image file = 1024-byte header region + app (2680 bytes for v1/v2).

## Boot cost (emulated instructions)

`python tests/renode/measure_boot.py`. Renode's executed-instruction counter from bootloader `main` to app
`main` on a steady-state boot (metadata already valid, one image verified). These are emulated
*instructions*, not cycles and not wall-clock time: Renode does not model Cortex-M4 pipeline timing, so
the figure is a relative measure only.

| Bootloader build | Instructions to reach the app |
|---|---:|
| With SHA-256 + ECDSA P-256 verification | 7,957,332 |
| Without signature check (measurement-only build, never shipped) | 97,250 |
| Signature verification cost | 7,860,082 (98.8% of boot) |

The measurement-only build (`-DSF_MEASURE_NO_SIGNATURE`) is 2,260 bytes against 6,868 bytes for the real
bootloader, so SHA-256 plus micro-ecc account for about 4.6 KB of code.

## Not measured

- Wall-clock boot time, cycle-accurate timing, and current/power draw (no hardware).
- Renode-level power cuts cover 15 chosen points, not every instruction boundary. The exhaustive coverage is
  the host sweep, which runs the same source but the host build, not the ARM binary.
