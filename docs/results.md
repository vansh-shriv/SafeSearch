# Results

All numbers below were measured in this repository. Regenerate them with the commands in `README.md`.
Nothing here is a hardware measurement. Current/power draw was not measured: this project targets logic and
fail-safety, not power characterisation.

## Exhaustive power-cut sweep (host, real code, real crypto)

`mingw32-make -C tests/unit sweep` (about 25 s). Production boot, install, confirm and metadata code, with real
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
| `ota_install` | 5 | 2715 | 2715 | 2714 / 1 |
| `confirm` | 2 | 24 | 24 | 0 / 24 |
| `boot_first_boot` | 2 | 24 | 24 | 24 / 0 |
| `boot_trial_increment` | 2 | 24 | 24 | 0 / 24 |
| `boot_revert` | 2 | 24 | 24 | 24 / 0 |
| `recovery_install` | 11 | 2715 | 2715 | 0 / 932 |
| **Total** | | **5526** | **5526** | |

The single v2 outcome in `ota_install` is the un-cut control run. Every cut before the metadata commit
completes leaves the device on the old v1 image, which is the atomic behaviour intended.

`recovery_install` starts from a device with nothing bootable and cuts a serial-recovery install at every
flash mutation. After a cut the device is either bootable with the new verified image only (932 points, all
v2), or unbootable but still recoverable (the remaining points, boot version shown as 0): in every one of
those, re-running the recovery transfer succeeds and the device then boots v2.

### The harness was itself tested

A sweep that reports zero violations proves little unless it can fail. Injecting real defects:

| Injected defect | Violations found |
|---|---:|
| Installer erases the *active* slot instead of the inactive one | 2705 of 2707 points (device bricked or ratchet wrong) |
| Metadata rewritten in place instead of ping-pong | 88 (anti-rollback floor decreased) |
| Recovery writes the *active* slot instead of the other one | 2715 of 2715 recovery points |

Deliberately committing metadata before writing the image is not a violation: the bootloader's fallback
reverts a torn image, so that ordering is not safety-critical. The installer keeps the safe order anyway.

## Host unit tests

`mingw32-make -C tests/unit test`: 26 checks pass. They cover CRC-32, SHA-256 known-answer vectors (including
one million `a`), metadata ping-pong, corruption fallback, sequence wrap, header validation, and a
126-point power-cut sweep of the metadata write.

`mingw32-make -C tests/unit recovery`: 22 checks with real signatures. A full transfer installs and boots;
line noise, duplicated frames and a corrupted frame are tolerated; a forged image is NAKed with the
payload-hash reason and nothing is committed; a validly signed older image is refused (below the floor);
protocol violations (data before begin, absurd size, skipped offset, unknown type) are NAKed; and an
interrupted transfer never leaves a valid-looking header. Injecting defects: recovery writing the active
slot, or skipping the signature check before committing, each fail these tests.

## Fuzzing

`mingw32-make -C tests/fuzz run ITERS=50000`. Five property-checking targets over the production code:
arbitrary slot contents against the verifier, arbitrary metadata sectors, whole-device state into
`boot_decide` (real crypto), arbitrary blobs into `sf_install_update`, and arbitrary byte streams into serial
recovery. A violation aborts. The properties include "only a byte-exact genuine image ever verifies",
"boot never selects a slot below the floor", "the running slot is never modified", "only the target slot
and (on success) the metadata sectors are ever written" and "an unconfirmed image cannot loop forever".
Inputs are structure-aware (CRC and hash fixups) so the fuzzer reaches the hash and signature checks
instead of stopping at the unkeyed CRCs.

Final run, two RNG seeds of 50,000 iterations per target (1,000,000 iterations in total), no property violations:

| Target | Iterations | Distinct outcomes reached | Violations |
|---|---:|---:|---:|
| `image` | 100,000 | 7 | 0 |
| `metadata` | 100,000 | 2 | 0 |
| `boot` | 100,000 | 20 | 0 |
| `install` | 100,000 | 3 | 0 |
| `recovery` | 100,000 | 11 | 0 |

"Distinct outcomes" is the driver's own feedback signal (result codes and decision paths), not code coverage;
it saturates quickly, which is why the coverage table below is the better measure of reach.

Line coverage of the code under test reached by the portable driver (gcov, 6,000 iterations per target):

| File | Lines executed | Branches taken |
|---|---:|---:|
| `boot_logic.c` | 87.7% | 79.4% |
| `recovery.c` | 88.2% | 85.7% |
| `metadata.c` | 96.4% | 88.2% |
| `image_verify.c` | 92.3% | 81.8% |
| `image_crypto.c` | 95.5% | 91.7% |
| `safeflash_update.c` | 80.8% | 68.2% |

The unreached lines are flash-error returns the mock does not produce (covered by the power-cut sweep) and,
in `safeflash_app.c`, the watchdog-kick path that touches hardware. The driver is not coverage-guided; CI
additionally builds libFuzzer targets with clang, AddressSanitizer and UBSan and runs them for 45 s each.
Injecting defects, the fuzzers catch recovery targeting the wrong slot and the bootloader ignoring the
anti-rollback floor.

## Model check

`mingw32-make -C tests/model check` (about 90 s). An explicit-state model checker over the production
`boot_decide`, metadata, confirm and install code, with only the cryptography abstracted to an oracle over slot
contents (invalid, valid v1-v3, forged). It explores every reachable device state under every interleaving of
boot (app hangs, confirms, or installs v1/v2/v3/forged), a power cut at every flash mutation with several torn-write and
torn-erase variants, and an adversary that overwrites either slot with any content at rest.

| | |
|---|---:|
| Reachable device states | 10,750 |
| Transitions explored | 658,946 |
| of which with a power cut | 508,446 |
| Properties | 7 (S1-S5, L1, L2) |
| Violations | 0 |

Injecting five defects into the production code (floor ignored in the slot check, signature check skipped,
trial counter never incremented, metadata rewritten in place, confirm resetting the floor to 0), all five are
caught (`python tests/model/mutate.py`). Two mutants initially survived, which drove two improvements: a
tampering adversary (the floor check in the boot path is otherwise unobservable) and a progress property S5
(a ratchet that never rises violates no safety property).

## Static analysis

cppcheck 2.22 with `warning,portability,performance,style` is clean on the bootloader, app library and SHA-256
(vendored micro-ecc excluded). One suppression: startup code compares linker-script symbols, which cppcheck
cannot know are the same region.

## Renode tests (emulated STM32F4, real firmware binaries)

Robot Framework suites, run with `powershell -File sim/run_robot.ps1` (about 2 to 4 minutes for all 32 tests). The negative paths were checked: a wrong expected rejection reason, and a power-cut hook that never fires, both turn the suite red.

| Suite | Result | What it shows |
|---|---|---|
| `sim/robot/signature.robot` | 6/6 | Genuine image boots. Attack images with self-consistent CRCs (stale hash, stale signature, version bump, wrong key, zeroed signature) are each rejected for the expected reason and the device falls back to the genuine image |
| `sim/robot/trial.robot` | 3/3 | A good image confirms and survives past the watchdog window; the next boot is a normal one. A "bad" image that never confirms gets exactly 3 watchdog-reset trials, then the bootloader reverts to the previous slot and stays there |
| `sim/robot/ota.robot` | 4/4 | OTA v1 to v2 installs, boots in trial, confirms, and raises the floor to 2. The installer refuses a validly signed v1 below the floor. The bootloader refuses v1 below the floor even if metadata points at it. A bad-signature image is installed but rejected at boot and reverted |
| `sim/robot/powercut.robot` | 15/15 | Power cuts on the real ARM binary: a Renode hook on the flash driver freezes the CPU at a chosen point during an OTA install, the machine is reset, and the bootloader must recover. 14 cut points: before the target-slot erase, three payload word boundaries, four header word boundaries, before the metadata sector erase, and each of the 5 words of the metadata commit. Every cut boots the old v1 and never applies the update; the un-cut control boots v2 |
| `sim/robot/recovery.robot` | 4/4 | Serial recovery end to end over a socket wired to USART1, driven by `tools/recover.py`: with both slots unbootable the bootloader enters recovery and installs a signed image; a forged image is refused and the genuine one then installs; recovery can be requested on a healthy device and installs into the inactive slot; the request word is consumed so the next reset boots normally |

## Sizes (arm-none-eabi-size, `-Os`)

| Binary | text bytes |
|---|---:|
| Bootloader incl. recovery (of a 32 KB region; plus 1300 bytes of RAM) | 8044 |
| Demo app v1 / v2 | 1664 |
| Demo app v3 (never confirms) | 1460 |

Signed image file = 1024-byte header region + app (2688 bytes for v1/v2).

## Boot cost (emulated instructions)

`python tests/renode/measure_boot.py`. Renode's executed-instruction counter from bootloader `main` to app
`main` on a steady-state boot (metadata already valid, one image verified). These are emulated
*instructions*, not cycles and not wall-clock time: Renode does not model Cortex-M4 pipeline timing, so
the figure is a relative measure only.

| Bootloader build | Instructions to reach the app |
|---|---:|
| With SHA-256 + ECDSA P-256 verification | 7,920,121 |
| Without signature check (measurement-only build, never shipped) | 97,256 |
| Signature verification cost | 7,822,865 (98.8% of boot) |

The measurement-only build (`-DSF_MEASURE_NO_SIGNATURE`) removes only the boot-path signature call; recovery
still links the crypto, so its size is not a useful measure of crypto code size any more.

## Not measured

- Wall-clock boot time, cycle-accurate timing, and current/power draw (no hardware).
- Renode-level power cuts cover 14 chosen points, not every instruction boundary. The exhaustive coverage is
  the host sweep, which runs the same source but the host build, not the ARM binary.
- Coverage-guided fuzzing has not been run locally (MinGW has no libFuzzer or sanitizers); the libFuzzer +
  ASan/UBSan job exists in CI. The local runs use a portable mutation driver with outcome feedback.
- A bounded model checker such as CBMC was not used (no Windows package was available); the explicit-state
  checker above covers the same properties over an abstraction of the crypto and slot contents.
- The recovery receiver has no timeout and polls the UART; it was exercised at emulated speed, not at a
  real baud rate.
