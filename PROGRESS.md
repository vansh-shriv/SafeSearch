# SafeFlash progress log

Spec: `safeflash-spec-simulator.md`. Update this file at the end of every work session.

## Current status

**Phases 0-5 done and verified. Phase 6 in progress: host exhaustive fault sweep done (2,803 fault points, 0 violations, harness mutation-tested). Still to do: Renode-level power cuts on the real binary, CI, README/design/results docs, cycle-count metrics.**

## Environment (verified 2026-09-26)

| Tool | State |
|---|---|
| Renode 1.16.0 | `C:\Program Files\Renode\bin\Renode.exe` (not on PATH). Downloads the STM32F40x SVD on first run (network once, then cached) |
| Arm GNU Toolchain 12.2 (arm-none-eabi-gcc) | `C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\12.2 mpacbti-rel1\bin` (not on PATH; Makefile `TC_BIN`) |
| MinGW gcc 6.3 (32-bit) + `mingw32-make` | `C:\MinGW\bin` (not on PATH). Host unit tests, fault sweep, top-level build |
| Python 3.12 (conda `tf_env`) + `cryptography` 50.0.1 | Use `python -m pip`, NOT bare `pip` (different interpreter) |
| QEMU | Not installed (only needed as Renode fallback) |

Renode platform facts (`platforms/cpus/stm32f4.repl`): flash 2 MB @ `0x08000000` via `STM32F4_FlashController`, SRAM 256 KB @ `0x20000000`, IWDG @ `0x40003000`, USART1 @ `0x40011000`. Board: `platforms/boards/stm32f4_discovery.repl`.

## Architecture decisions

- Target: **STM32F4 (Renode `stm32f4.repl`, 2 MB flash)**.
- Flash map (`bootloader/src/flash_map.h`): bootloader sectors 0-1 (32 KB), metadata A/B in sectors 2/3, sector 4 unused, Slot A = sectors 5-7 @ `0x08020000`, Slot B = sectors 8-10 @ `0x08080000` (384 KB each). Image header padded to `0x400` at slot start, vector table after. Apps are position-dependent: one linker script per slot.
- All flash access goes through `flash_hal.h`. Target: `flash_stm32.c`. Host: `tests/unit/mock_flash.c` (can cut power at any mutation / byte, torn erase patterns, trace, snapshots).
- **Boot decision is hardware-independent** (`bootloader/src/boot_logic.c`) and is compiled unchanged into both the bootloader and the host fault sweep. `main.c` only does UART, watchdog arm and the jump.
- Trust model (`image_crypto.h`): ECDSA P-256 signature over SHA-256 of the first 48 header bytes (magic, version, size, crc32, payload sha256). Payload bound via the sha256 field. Public key baked in from `build/pubkey.c` (generated from gitignored `keys/private.pem`).
- Trial policy: bootloader persists `trial_count` BEFORE jumping, arms IWDG (`TRIAL_WDT_MS` 2000). In TRIAL, `sf_wdt_kick()` refuses to kick until `sf_confirm_healthy()` succeeds, so an image that runs but never confirms is still reset (bounded trial). After `MAX_TRIALS` (3) attempts the bootloader reverts to the other slot (which must verify and satisfy the floor). `BOOT_STATE_REVERT_PENDING` exists in the enum but is unused.
- Anti-rollback: `sf_confirm_healthy()` ratchets `min_allowed_version` up to the running image's version (same metadata commit as TRIAL->CONFIRMED). Bootloader rejects images below the floor; `sf_install_update()` refuses them up front.
- Update path (`app/src/safeflash_update.c`): erase inactive slot, program payload, program header last, then atomically commit metadata (active=new, TRIAL). Never touches the running slot. It does not verify the signature; the bootloader does and reverts on failure (found by mutation testing: the fallback also makes the write order non-critical for safety, but the design keeps the safe order anyway).
- Simulated OTA transport: host drops `{'STGE', len, image}` into RAM at `0x20020000`; the app installs it and issues SYSRESETREQ. Documented simplification: transport is out of scope, the update mechanism is under test.
- Slot images by `tools/sign_image.py`; keys by `tools/keytool.py`; test metadata blobs by `tools/mkmeta.py`.

## Done

- Phase 0-1: toolchain + Renode running; linker scripts, startup, UART, bootloader jump; Makefile
- Phase 2: ping-pong metadata (`metadata.c`), STM32F4 flash driver, slot selection + fallback. Verified: flash survives `machine Reset`
- Phase 3: SHA-256 (own, known-answer tested incl. 1M 'a'), vendored micro-ecc P-256 (`crypto/micro-ecc`, upstream commit in `VENDORED.txt`), sign/keygen tools. `tests/renode/test_signature.py` 6/6 (attack images with self-consistent CRCs: stale hash, stale signature, version bump, wrong key, zero signature)
- Phase 4: trial boot, confirm, IWDG revert. `tests/renode/test_trial.py` 12/12
- Phase 5: installer, ratchet, OTA. `tests/renode/test_ota.py` 8/8 (OTA v1->v2 with trial + confirm + floor 2; installer refuses signed v1 below floor (rc -3); bootloader refuses v1 below floor even if metadata points at it; evil OTA with bad signature installed then rejected by bootloader and reverted)
- Phase 6 (host): `tests/unit/fault_sweep.c` (`mingw32-make -C tests/unit sweep`, ~12 s). Real production code + real crypto vs mock flash, power cut at EVERY flash mutation and every byte boundary inside each write and 3 torn-erase states each. Scenarios: OTA install (2,707 points), confirm, bootloader first-boot init, trial-counter increment, revert commit (24 each). Total **2,803 distinct fault points, 0 violations**. Invariants: always boots a verified image (never bricked); image is old or new only; metadata valid after boot; floor never decreases and never exceeds running version; converges (confirmed or reverted). Every (cut point -> outcome) row is in `build/fault_sweep.csv`; `tools/summarize_sweep.py` prints the table
- **Harness validated by mutation testing** (so 0 violations is meaningful): installer erasing the ACTIVE slot -> 2,705/2,707 violations (bricked); metadata rewritten in place instead of ping-pong -> 88 violations (floor decreased). Misordering install (metadata before image write) is NOT a violation because the bootloader fallback reverts a torn image; that is by design

## In progress / next

- [ ] Renode-level power cuts on the real binary (PC hook on the flash driver's word-program function, reset at chosen addresses), to cross-check the host sweep on the actual firmware
- [ ] GitHub Actions CI (unit tests + sweep on host; Renode suites need Renode installed on the runner)
- [ ] Metrics: boot time and verify cost in emulated cycles/instructions (not yet measured; no numbers claimed). Bootloader/app sizes are real: bootloader ~6.7 KB (of 32 KB), apps ~1.1-1.6 KB
- [ ] `README.md`, `docs/design.md` (with explicit simulator-only scope section), `docs/results.md`
- [ ] Optionally convert Python-driven Renode suites to Robot Framework (`sim/robot/`)

## Known caveats

- Renode's flash controller ignores PSIZE and SR error bits ("Unhandled write" warnings), so flash error paths in `flash_stm32.c` are not exercised.
- Renode programs a word atomically, so torn-word programming is only modelled in the host sweep (byte-granular prefix, a superset of real word-granular behaviour). Torn-erase patterns are modelled host-side only.
- Host sweep does not nest faults (a second cut during the recovery boot after a first cut). The recovery boot's own metadata writes are covered individually by the `boot_*` scenarios.
- Metadata is CRC-protected, not authenticated: an attacker who can write flash directly can forge it (including the floor). Out of scope, same as the spec's threat model (image authenticity, not flash-bus tampering).
- Renode speed: a spinning app costs wall time per emulated second (~2-3 s wall per emulated s). Never hit peripheral registers every loop iteration (a per-iteration IWDG kick made 4 emulated s take 166 s).
- Shell: long bash heredocs with backslashes are unreliable here (a `\\n` became a real newline); use the Write/Edit tools for multi-line files. Use `cmd /c "mingw32-make 2>&1"` in PowerShell.

## Commands that work today

```
# PowerShell, from repo root
$env:Path = "C:\MinGW\bin;" + $env:Path
mingw32-make -C tests/unit test        # host unit tests (26 checks)
mingw32-make                           # build firmware into build/ (creates keys/private.pem on first run)
mingw32-make -C tests/unit sweep       # exhaustive power-cut sweep, writes build/fault_sweep.csv
python tools/summarize_sweep.py        # results table from the CSV
powershell -NoProfile -File sim/run.ps1 -Seconds 2     # boot in Renode, print UART
python tests/renode/test_signature.py  # attack images under Renode (needs `make` first)
python tests/renode/test_trial.py      # trial/confirm/watchdog/revert under Renode (~3-4 min)
python tests/renode/test_ota.py        # OTA install, ratchet, anti-rollback under Renode (~3 min)
```
