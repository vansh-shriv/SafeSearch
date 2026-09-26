# SafeFlash progress log

Spec: `safeflash-spec-simulator.md`. Update this file at the end of every work session.

## Current status

**Phases 0-6 done and verified; Renode suites ported to Robot Framework; then (no-board hardening pass) serial recovery mode, fuzzing and model checking added and verified locally.** The earlier CI jobs (host, renode) passed on GitHub; the CI changes from the hardening pass (new `analysis` job with cppcheck, sanitizers and libFuzzer, plus model-check/recovery/fuzz steps in `host`, and the recovery suite in `renode`) have **not been run on a runner yet**.

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
- Phase 3: SHA-256 (own, known-answer tested incl. 1M 'a'), vendored micro-ecc P-256 (`crypto/micro-ecc`, upstream commit in `VENDORED.txt`), sign/keygen tools. `sim/robot/signature.robot` 6/6 (attack images with self-consistent CRCs: stale hash, stale signature, version bump, wrong key, zero signature)
- Phase 4: trial boot, confirm, IWDG revert. `sim/robot/trial.robot` 3/3
- Phase 5: installer, ratchet, OTA. `sim/robot/ota.robot` 4/4 (OTA v1->v2 with trial + confirm + floor 2; installer refuses signed v1 below floor (rc -3); bootloader refuses v1 below floor even if metadata points at it; evil OTA with bad signature installed then rejected by bootloader and reverted)
- Phase 6 (host): `tests/unit/fault_sweep.c` (`mingw32-make -C tests/unit sweep`, ~12 s). Real production code + real crypto vs mock flash, power cut at EVERY flash mutation and every byte boundary inside each write and 3 torn-erase states each. Scenarios: OTA install (2,707 points), confirm, bootloader first-boot init, trial-counter increment, revert commit (24 each). Total **2,803 distinct fault points, 0 violations**. Invariants: always boots a verified image (never bricked); image is old or new only; metadata valid after boot; floor never decreases and never exceeds running version; converges (confirmed or reverted). Every (cut point -> outcome) row is in `build/fault_sweep.csv`; `tools/summarize_sweep.py` prints the table
- **Harness validated by mutation testing** (so 0 violations is meaningful): installer erasing the ACTIVE slot -> 2,705/2,707 violations (bricked); metadata rewritten in place instead of ping-pong -> 88 violations (floor decreased). Misordering install (metadata before image write) is NOT a violation because the bootloader fallback reverts a torn image; that is by design

- Phase 6 (Renode): `sim/robot/powercut.robot` 15/15 on the real ARM binary (Test Template, 14 cuts + control). A Renode PC hook on `program_word` / `flash_erase_sector` freezes the CPU at a chosen address (PC redirected into `Default_Handler`'s `for(;;)`), then `machine Reset`. Points: before target-slot erase, 3 payload words, 4 header words, before metadata erase, all 5 metadata-commit words. Every cut boots old v1; the control boots v2. A cut that never fires lets the install complete, which `Uart Should Not Show APP: install ok` catches (verified with a deliberately broken test)
- Phase 6 (metrics): `tests/renode/measure_boot.py`: 7,957,332 emulated instructions from bootloader entry to app entry (steady state) with signature verification vs 97,250 without; verify = 98.8% of boot. Instructions, not cycles. Measurement-only bootloader: `mingw32-make BUILD=build_nosig EXTRA_CFLAGS=-DSF_MEASURE_NO_SIGNATURE build_nosig/bootloader.elf` (`SF_MEASURE_NO_SIGNATURE` must never be used in a real build)
- Docs: `README.md`, `docs/design.md` (has the simulator-only scope section), `docs/results.md`
- CI: `.github/workflows/ci.yml` has a `host` job (build firmware, unit tests, exhaustive sweep, on ubuntu) and a `renode` job (latest Linux portable Renode, Robot suites; required, green). Run results reported by the user (the agent cannot read Actions results; the repo API returns 403 unauthenticated): the workflows for `07e2698` (Renode power-cut suite, boot-cost), `c2ffe4c` (untrack build artifact) and `389e3fc` (Robot port + Renode job) passed, so the host job works on ubuntu with the current gcc (no `-Werror` breakage, no `python` vs `python3` problem). The workflow for `5fac5b5` failed in the sweep step: that commit accidentally contained a Windows-built `tests/unit/uecc_host.o`, which make did not rebuild on Linux (i386 object, Windows `Crypt*` symbols). Fixed by `c2ffe4c` (untracked, `*.o` ignored); the red run for `5fac5b5` stays in history
  - The `renode` job (Robot suites on the latest Linux portable Renode) was confirmed green by the user on `389e3fc`, so `continue-on-error` was removed and it is now a required job. It downloads `renode-latest`; if a future Renode release breaks the suites, pin the URL to a known-good version

### Hardening pass (2026-09-27): recovery, fuzzing, model checking

- **Serial recovery mode**: `bootloader/src/recovery.{c,h}` (hardware-independent), `tools/recover.py` (host side, socket or serial). Entered when `boot_decide` fails (was: halt) or when the RAM request word holds `RECOVERY_REQUEST_MAGIC` (`sf_request_recovery()` in the app library). Protocol: `0xA5 | type | len | payload | crc32`, stop-and-wait, BEGIN/DATA/END/ABORT, ACK/NAK, idempotent retransmits, resync after noise. Header held in RAM and written last; verification identical to a normal boot (incl. version floor) before any metadata commit; only the non-active slot is written. Top 16 bytes of RAM reserved for the request word in all three linker scripts (`_estack` lowered). Bootloader now 8044 B text + 1300 B bss (of 32 KB)
- Tests for it: `mingw32-make -C tests/unit recovery` (22 checks, real signatures); sweep scenario `recovery_install` (2715 cut points, 0 violations; either still bootable with only the verified image or unbootable-but-resumable); fuzz target `recovery`; `sim/robot/recovery.robot` 4/4 in Renode (both slots bad -> recovery -> install; forged image refused then genuine installs; forced recovery on a healthy device; request word consumed). Mutation-tested: recovery writing the active slot -> 2715 sweep violations + unit failure; skipping the signature check -> 4 unit failures
- **Fuzzing** (`tests/fuzz`): 5 property-checking targets (`fuzz_image`, `fuzz_metadata`, `fuzz_boot`, `fuzz_install`, `fuzz_recovery`) in `fuzz_targets.c`, shared by a portable mutation driver (`fuzz_driver.c`, deterministic seeds, outcome feedback, structure-aware CRC/hash/frame fixups) and libFuzzer (`fuzz_libfuzzer.c`). gcov coverage from the driver: boot_logic 87.7% lines, recovery 88.2%, metadata 96.4%, image_verify 92.3%, image_crypto 95.5%, safeflash_update 80.8%. Coverage measurement found real gaps (trial-counter path, hash/signature failure results, rollback refusal never reached), fixed by building those cases deliberately. Mutation-tested: recovery wrong slot and floor ignored in the slot check are both caught. `SAN=1` builds the unit tests with ASan/UBSan (Linux only)
- **Model checking** (`tests/model/model_check.c`): explicit-state, production `boot_decide`/metadata/confirm/install with crypto abstracted to an oracle over slot contents {invalid, v1, v2, v3, forged}; actions boot+{hang, confirm, install v1/v2/v3/forged}, each with a power cut at every mutation (torn writes, 3 torn-erase patterns), plus a tampering adversary (any slot overwritten at rest). 10,750 reachable states, 658,946 transitions (508,446 with a power cut), 7 properties S1-S5, L1, L2, all hold (~90 s). `tests/model/mutate.py`: 5 injected defects, all caught. Two mutants first survived (floor check, confirm lowering floor): both were equivalent in the first model (unreachable without an adversary / no progress property), fixing the model (tamper actions, property S5) rather than the code. CBMC was not available (not on winget/conda-forge for Windows), so no bounded-model-checking harness was written
- **Static analysis**: cppcheck 2.22 (`warning,portability,performance,style`) clean after fixing const-correctness, two parameters shadowing `slot_addr()`, and a redundant assignment; one narrow suppression for linker-symbol pointer comparison in the startup files
- Bugs/observations found on the way: (1) the bootloader Makefile rule had no header dependencies, so editing `uart.h` did not rebuild `main.o` (fixed); (2) Renode socket terminals emulate telnet by default, which corrupts binary frames: create with `false` as the 3rd argument; (3) polling a UART register in a tight loop made one 3-emulated-second wait take 547 s of wall time; a short delay between polls (realistic and harmless on hardware) brought it to ~20 s; (4) `SF_MEASURE_NO_SIGNATURE` no longer shrinks the bootloader because recovery links the crypto
- CI additions (**not yet run on a runner**): `host` job gains recovery tests, model check, and a 20k-iteration fuzz smoke run; new `analysis` job (cppcheck, unit/recovery/sweep under ASan+UBSan via `SAN=1`, libFuzzer builds with clang, seed corpora from `tests/fuzz/gen_corpus.py`, 45 s per target); `renode` job runs `recovery.robot` too. Likely trouble spots: distro cppcheck version differing from 2.22, ASan complaining about something MinGW could not, `gen_corpus.py` needing Python >= 3.9 (`randbytes`)

## In progress / next

- [ ] Look at the first GitHub run of the new CI jobs and fix whatever breaks (see the CI note above)
- [ ] Optional: pin the CI Renode download to a specific version instead of `renode-latest` for reproducible runs
- [ ] Optional: recovery hardening: a receive timeout / watchdog, interrupt-driven RX with a ring buffer, resumable transfers, and a bounded model check (CBMC) of `boot_decide` over symbolic metadata
- [ ] Hardware (if a board is ever available): real flash error paths, DWT cycle counts, RDP/write-protect option bytes, power measurement

## Known caveats

- Renode's flash controller ignores PSIZE and SR error bits ("Unhandled write" warnings), so flash error paths in `flash_stm32.c` are not exercised.
- Renode programs a word atomically, so torn-word programming is only modelled in the host sweep (byte-granular prefix, a superset of real word-granular behaviour). Torn-erase patterns are modelled host-side only.
- Host sweep does not nest faults (a second cut during the recovery boot after a first cut). The recovery boot's own metadata writes are covered individually by the `boot_*` scenarios.
- Metadata is CRC-protected, not authenticated: an attacker who can write flash directly can forge it (including the floor). Out of scope, same as the spec's threat model (image authenticity, not flash-bus tampering).
- Renode Python hooks (`sysbus.cpu AddHook <addr> "<python>"`): `self` is the CPU. Calling `self.machine.Reset()` inside a hook crashes Renode (fatal error). Assigning `self.PC = <int>` crashes too ("expected RegisterValue"); use `self.PC = type(self.PC).Create(addr, 32)`. Registers: `self.GetRegister(n).RawValue`. Instruction count: `self.ExecutedInstructions`, which restarts at each machine reset. Renode logs a spurious "Tried to erase flash, but MER and SER are reset" warning although erases do take effect (verified: erased sectors read 0xFF; Renode flash starts as 0x00, not 0xFF).
- `sim/run.ps1` (ad-hoc) logs UART to `%TEMP%\safeflash\uart.log`; the Robot suites use Renode's terminal tester instead. Robot 6.1 needs `PYTHONIOENCODING=utf-8` (set by `sim/run_robot.ps1`). In Robot, relative paths in `sim/boot.resc` resolve after `path add "<repo>"`. `Wait For Line On Uart` timeouts are Renode virtual seconds and match lines containing the text; a boot with signature verification takes ~78 virtual ms.
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
mingw32-make -C tests/unit recovery    # serial recovery tests
mingw32-make -C tests/model check      # model check (~90 s); python tests/model/mutate.py to mutation-test it
mingw32-make -C tests/fuzz run ITERS=50000   # fuzz driver, 5 targets (`coverage` target for a gcov report)
powershell -NoProfile -File sim/run_robot.ps1   # Robot suites under Renode: signature, trial, ota, powercut, recovery (~2-4 min). One-time: py -3 -m pip install robotframework==6.1 robotframework-retryfailed==0.2.0 psutil pyyaml telnetlib3
python tests/renode/measure_boot.py    # instruction counts (needs the build_nosig bootloader, see script header)
```
