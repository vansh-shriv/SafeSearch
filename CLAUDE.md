# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

`safeflash-spec-simulator.md` (PRD + technical design + build roadmap) is the source of truth; follow its phase order (§6) and repo layout (§7). **`PROGRESS.md` is the running log of what is done, in progress, and blocked. Read it first and update it at the end of each work session.**

## Commands

Host unit tests (bootloader logic against a mock flash). MinGW is not on PATH by default:
```
$env:Path = "C:\MinGW\bin;" + $env:Path
mingw32-make -C tests/unit test
```
Firmware build (bootloader + slot A/B app images into `build/`) and a headless Renode boot that prints USART1:
```
mingw32-make
powershell -NoProfile -File sim/run.ps1 -Seconds 2
python tests/renode/test_signature.py   # attack images under Renode; needs `mingw32-make` first
```
`sim/run.ps1` also takes `-Steps "<monitor cmds>"` (e.g. `machine Reset; sysbus.cpu VectorTableOffset 0x08000000; emulation RunFor '2';`) and `-SlotA/-SlotB build/x.img` to swap images. `make` generates a dev keypair in gitignored `keys/` on first run; never commit it. Signature trust model is in `bootloader/src/image_crypto.h`.
Renode (`C:\Program Files\Renode\bin\Renode.exe`) and the Arm toolchain (path in the `Makefile` as `TC_BIN`) are not on PATH. Renode `@path` arguments break on spaces in this repo's path, and relative file-backend paths are not resolved against the repo, so `sim/run.ps1` logs UART to `%TEMP%\safeflash`. Use `python -m pip`, not bare `pip` (different interpreter). Signing tools and Robot suites do not exist yet (see `PROGRESS.md`).

Core bootloader logic (`bootloader/src/metadata.c`, `image_verify.c`, `crc32.c`) is hardware-independent and reaches flash only through `flash_hal.h`. Keep it that way so the same code is unit-tested on host and run on target.

## What it is

A secure bootloader with signed A/B OTA for an ARM Cortex-M (STM32F4) MCU, run **entirely under Renode** (fallback: QEMU Cortex-M machine driven via GDB stub). No physical board. Firmware logic must stay hardware-agnostic so it runs unchanged on a real board later.

## Architecture (spans multiple files)

- **Flash layout**: bootloader sectors, two ping-pong metadata sectors (A/B), then Slot A and Slot B application regions. Derive the exact sector map from the Renode platform `.repl` file for the chosen part, not the datasheet. The emulated part must match.
- **Image header** (`image_header_t`): magic `0x53414645` ("SAFE"), version, size, CRC32, SHA-256, 64-byte ECDSA P-256 signature, header CRC32.
- **Metadata** (`boot_metadata_t`): magic, monotonic `seq`, `active_slot`, `boot_state` (NORMAL/TRIAL/CONFIRMED/REVERT_PENDING), `trial_count`, `min_allowed_version` (anti-rollback floor), CRC32. Two copies are written alternately and the valid one with the highest `seq` wins, so a power cut mid-write never leaves zero valid copies.
- **Boot flow**: load metadata → pick active slot → validate header → check version ≥ `min_allowed_version` → verify SHA-256 + ECDSA → jump (set VTOR) in TRIAL or NORMAL. The app calls `confirm_healthy()`. Otherwise the IWDG trial watchdog resets, and after MAX_TRIALS the bootloader reverts `active_slot`.
- **OTA is simulated**: a host script writes the signed image straight into the inactive slot's emulated flash. The transport is deliberately out of scope, or optionally a minimal UART receiver. Updates must never touch the running slot.
- **Crypto**: micro-ecc (or trimmed mbedTLS) vendored in `crypto/`. `tools/sign_image.py` signs images offline. Crypto and metadata/header/version logic are unit-tested on the host with flash mocked as memory buffers.

## Testing model

- Host unit tests (metadata ping-pong, header validation, version logic).
- Renode Robot Framework suites in `sim/robot/`: happy path, OTA, bad signature, rollback (reset MAX_TRIALS+1 times), anti-rollback.
- **Fault injection is exhaustive, not sampled**: reset the machine at every chosen PC/instruction-count point in the update routine (before/mid erase, before/mid write, around the metadata write, and so on). Assert the device always boots some signature-verified image and that metadata is never left with no valid copy. Log each cut point and its outcome to CSV.

## Constraints to preserve

- No current/power measurement. Say so plainly in the README and never invent a number. Cycle counts may be reported only if labeled "cycles, not measured current".
- Report host timing and target (emulated) timing separately and label them.
- Docs must include an explicit "simulator-only scope" section in `docs/design.md`.
- If falling back from Renode to QEMU, record the reason in the design doc.
