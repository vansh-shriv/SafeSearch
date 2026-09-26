# SafeFlash progress log

Spec: `safeflash-spec-simulator.md`. Update this file at the end of every work session.

## Current status

**Phases 0-3 done and verified under Renode (boot, metadata slot selection/fallback, signed images).**
Next up: Phase 4 (trial boot, `confirm_healthy()`, IWDG watchdog, automatic revert).

## Environment (verified 2026-09-26)

| Tool | State |
|---|---|
| Renode 1.16.0 | `C:\Program Files\Renode\bin\Renode.exe` (not on PATH). Downloads the STM32F40x SVD on first run (needs network once, then cached) |
| Arm GNU Toolchain 12.2 (arm-none-eabi-gcc) | `C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\12.2 mpacbti-rel1\bin` (not on PATH; Makefile has it as `TC_BIN`) |
| MinGW gcc 6.3 + `mingw32-make` | `C:\MinGW\bin` (not on PATH). Host unit tests + top-level build |
| Python 3.12 (conda `tf_env`) + `cryptography` 50.0.1 | Installed. Use `python -m pip`, NOT bare `pip` (that is a different Python) |
| QEMU | Not installed (only needed as Renode fallback) |

Renode platform facts (`platforms/cpus/stm32f4.repl`): flash 2 MB @ `0x08000000` via `STM32F4_FlashController`, SRAM 256 KB @ `0x20000000`, IWDG @ `0x40003000`, USART1 @ `0x40011000`. We use `platforms/boards/stm32f4_discovery.repl`.

## Decisions made

- Target part: **STM32F4 (Renode `stm32f4.repl`, 2 MB flash)**. Spec §8 open decision 1 resolved.
- Flash map (`bootloader/src/flash_map.h`): bootloader sectors 0-1 (32 KB), metadata A/B in sectors 2/3, sector 4 unused, Slot A = sectors 5-7 @ `0x08020000` (384 KB), Slot B = sectors 8-10 @ `0x08080000` (384 KB). Image header at slot start padded to `0x400`, vector table follows. Apps are position-dependent: separate linker scripts per slot.
- Flash access goes through `flash_hal.h` (read / erase_sector / write). Host tests use a RAM mock, target uses the STM32F4 flash controller.
- Metadata CRC covers all fields before `crc32`. `seq` comparison is wrap-safe.
- Payload CRC streamed in 256-byte chunks to keep bootloader RAM small.
- Freestanding bootloader ships its own `memcpy/memset/memcmp` (`libc_min.c`) since the compiler emits calls for struct copies.
- Slot images are built by `tools/pack_image.py` (CRC-only header, sha256/signature zeroed). Phase 3 replaces it with `sign_image.py`.
- Robot tests should use Renode's terminal tester on USART1. Ad-hoc runs use a file backend in `%TEMP%\safeflash` because Renode `@path` syntax cannot handle spaces in this repo's path, and relative file-backend paths are not resolved against the repo.

## Done

- [x] Read spec, wrote `CLAUDE.md`
- [x] Installed Arm toolchain + Python `cryptography`
- [x] Repo scaffold per spec §7
- [x] `crc32`, `metadata.c` (ping-pong), `image_verify.c` (`image_check_basic`), all hardware-independent
- [x] Host unit tests: 22 checks pass, including exhaustive power-cut sweep of `metadata_store` (126 cut points, 0 bad)
- [x] Linker scripts (bootloader, app slot A/B), startup, UART helper, bootloader `main.c`, demo app
- [x] Top-level `Makefile` builds bootloader (868 B) + app v1 for slot A + app v2 for slot B, wrapped as `.img`
- [x] `sim/boot.resc` + `sim/run.ps1`: **verified under Renode**: reset -> BL validates slot A (CRC) -> jumps -> app prints its version:
  ```
  SafeFlash BL
  BL: slot A: ok
  BL: jumping to version 0x00000001
  APP: running, version 1
  ```

- [x] Phase 2: `flash_stm32.c` (unlock, sector erase, word programming with 0xFF-padded head/tail) works against Renode's `STM32F4_FlashController`
- [x] Phase 2: bootloader loads metadata, initialises it on first boot (slot A, NORMAL, floor 0), falls back to the other slot if the active one fails checks and persists the switch
- [x] Verified under Renode (ad-hoc, via `sim/run.ps1 -Steps`): flash contents survive `machine Reset`; second boot reads metadata seq 1 without re-init; corrupting Slot A payload -> "bad image crc" -> falls back to B, seq 2 stored, next reset boots B directly. Resolves the earlier open question about flash surviving reset. Note: after `machine Reset` the script must re-set `sysbus.cpu VectorTableOffset 0x08000000`
- [x] Repo pushed to https://github.com/vansh-shriv/SafeSearch.git (branch `main`). User authorised periodic commits there. Commit + push at the end of each milestone

- [x] Phase 3: SHA-256 (`crypto/sha256.c`, own implementation, known-answer tests incl. 1M 'a') + vendored micro-ecc P-256 verify (`crypto/micro-ecc/`, upstream commit in `VENDORED.txt`, compiled with `-w`, unmodified)
- [x] `tools/keytool.py` (gen keypair into gitignored `keys/`, emit `build/pubkey.c`), `tools/sign_image.py` (replaces `pack_image.py`). `make` auto-generates a dev key if none exists
- [x] Trust model (documented in `image_crypto.h`): signature = ECDSA P-256 over SHA-256 of the first 48 header bytes (magic, version, size, crc32, payload sha256). Payload bound via the sha256 field. So version/size/hash tampering breaks the signature
- [x] Bootloader now runs structural checks, then payload hash + signature, before any jump. Size with crypto: 6.4 KB of 32 KB
- [x] `tests/renode/test_signature.py`: 6/6 pass under Renode. Attack images have all unkeyed CRCs recomputed so only crypto can reject them: stale hash, stale signature, version bump, wrong key, zeroed signature (+ valid control). Each is rejected with the expected reason and the bootloader falls back to genuine v2 in slot B
- [x] Host unit tests now 26 checks (SHA-256 vectors added)

## In progress / next

- [ ] Phase 4: `confirm_healthy()` library, TRIAL boot state, IWDG watchdog, trial counter and automatic revert. Under Renode: bad v3 that never confirms reverts to previous slot after MAX_TRIALS
- [ ] Turn the Renode checks into Robot Framework tests (`sim/robot/`) using the terminal tester; currently Python driving `sim/run.ps1`
- [ ] Measure boot time / verify cost in emulated cycles (Phase 6 metrics). Not measured yet, so no numbers are claimed

## Later (per spec §6)

- Phase 5: staged-image OTA simulation, anti-rollback ratchet
- Phase 6: Robot Framework suites, exhaustive Renode fault injection, CSV results, GitHub Actions CI, README/design/results docs

## Known caveats

- Mock flash models an interrupted erase as random garbage and an interrupted write as a byte-prefix. Real STM32 programming is word-granular, so this is a superset for byte writes.
- Unit tests run only on host; Renode coverage so far is the single happy-path boot above.
- Renode's flash controller may not model real erase/program timing or the "code stalls while flash busy" behaviour, so partial-erase/partial-write fault windows need to be injected by us (reset at chosen PC/instruction counts), not expected to occur naturally.
- Shell: long bash heredocs fail here; use the Write tool for multi-line files. Use `cmd /c "mingw32-make 2>&1"` in PowerShell to avoid stderr being turned into errors.

## Commands that work today

```
# PowerShell, from repo root
$env:Path = "C:\MinGW\bin;" + $env:Path
mingw32-make -C tests/unit test        # host unit tests
mingw32-make                           # build firmware into build/ (creates keys/private.pem on first run)
powershell -NoProfile -File sim/run.ps1 -Seconds 2     # boot in Renode, print UART (ECDSA verify needs ~1-2 simulated s)
python tests/renode/test_signature.py  # attack-image tests under Renode (needs `make` first)
```
