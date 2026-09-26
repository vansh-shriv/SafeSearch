# SafeFlash — Secure Bootloader with Signed A/B OTA
## Simulator-Only Specification (PRD + Technical Design + Build Roadmap)

---

## 0. Document purpose

This is the emulator-only version of SafeFlash: everything runs on your laptop, no board required. The target platform is still an ARM Cortex-M MCU, but instead of flashing a physical chip, you run the exact same firmware binaries under **Renode** (preferred) or **QEMU**, which model the CPU, flash, RAM, and peripherals closely enough that your bootloader code doesn't know the difference. This keeps every piece of the security/reliability design intact — signature verification, dual-slot updates, rollback, anti-rollback — while replacing "plug in a board" with "run a script."

Target: **STM32F4** (e.g., F407 or F446), which Renode has a built-in platform description for. If you later get a real board, this firmware runs on it with no changes to the bootloader logic — only the build/flash tooling changes.

---

## 1. Product requirements (PRD)

Unchanged from the hardware version in spirit — same problem, same goals:

1. No image runs unless its signature verifies against a baked-in public key.
2. An update never touches the currently running firmware; it always lands in the inactive slot.
3. A newly flashed image gets a bounded trial period; if it doesn't confirm itself healthy, the bootloader reverts automatically.
4. The system survives a power loss at any point in the update process.
5. An attacker cannot force the device back to an older, vulnerable, signed image (anti-rollback).
6. Everything is measurable, and "power loss" is tested exhaustively, because in the emulator you can do this hundreds of times in minutes instead of hours.

**What changes because there's no board:** "power loss" becomes "emulator reset/kill at an arbitrary instruction," and "measure current draw" is dropped from goals — it's a hardware-only metric with no honest emulated equivalent (say so plainly in your README rather than inventing a number). Everything else in the PRD holds.

---

## 2. Why Renode instead of raw QEMU

Renode (by Antmicro, used by the Zephyr and Renesas ecosystems) is built specifically for this kind of project:
- It models flash, GPIO, UART, and timers for real STM32 parts out of the box, so your bootloader's flash erase/write calls and UART logging work unmodified.
- It has a **Robot Framework** test integration, so you can script "start emulation, run to a breakpoint, kill the machine, restart, assert X" — this is exactly the fault-injection harness you need, and it's far easier to drive than QEMU's monitor interface for this purpose.
- It supports **deterministic execution and snapshots**, which is what makes exhaustive power-cut testing tractable: you can resume from a saved point-in-time state rather than replaying from boot every time.
- It's free, scriptable in Python/Robot, and has an active Zephyr/embedded community, so "I used Renode for HIL-style testing" is itself a legible, current skill to name on a resume.

If Renode setup proves painful for the exact STM32 variant + peripheral set you need, fall back to QEMU's `mps2-an385`/`netduinoplus2`-class Cortex-M machine and drive it via its GDB stub + monitor commands for the same reset/inspect loop. Note the fallback in your design doc if you take it, with the reason.

---

## 3. System architecture

All of §2 (flash memory map, image header, metadata, bootloader state machine, update flow) from the original hardware spec **is unchanged** — copy it forward as-is. It is written at the level of flash addresses, structs, and state transitions, none of which care whether the flash is silicon or an emulated memory region. Reproduced here for completeness:

### 3.1 Flash memory map (STM32F4, 512 KB or 1 MB depending on exact part Renode models — check the platform's `.repl` file for the exact flash size and sector layout it exposes, and derive your map from that rather than the datasheet, since the emulated part must match)

```
Sector 0   Bootloader (SafeFlash)
Sector 1   Bootloader (cont'd) / reserved
Sector 2   Metadata copy A
Sector 3   Metadata copy B
Sectors 4+ Slot A (application)
Sectors N+ Slot B (application)
```

### 3.2 Image header
```c
typedef struct {
    uint32_t magic;            // 0x53414645 ("SAFE")
    uint32_t version;
    uint32_t image_size;
    uint32_t image_crc32;
    uint8_t  sha256[32];
    uint8_t  signature[64];
    uint32_t header_crc32;
} image_header_t;
```

### 3.3 Metadata (ping-pong, power-safe)
```c
typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint8_t  active_slot;
    uint8_t  boot_state;   // NORMAL, TRIAL, CONFIRMED, REVERT_PENDING
    uint8_t  trial_count;
    uint8_t  reserved;
    uint32_t min_allowed_version;
    uint32_t crc32;
} boot_metadata_t;
```

### 3.4 Bootloader state machine
Identical to the hardware spec: load metadata → pick active slot → validate header → check version floor → verify SHA-256 + ECDSA signature → boot in TRIAL or NORMAL state → application confirms healthy or trial watchdog reverts. No change.

### 3.5 What's simulated instead of physical

| Hardware version | Simulator version |
|---|---|
| ST-Link flashes the board | Renode loads your `.elf`/`.bin` into the emulated flash region via its `sysbus LoadELF` / `LoadBinary` commands |
| Independent watchdog (IWDG) hardware timer | Renode models the STM32 IWDG peripheral directly — same registers, same behavior, your code is unchanged |
| USB relay cuts board power | A Renode/Robot Framework script issues `machine Reset` or `Emulation Clear` mid-run, at a controlled point (see §5.3) |
| UART to a serial terminal | Renode's UART peripheral, read via its virtual terminal or piped to a PTY your test scripts read from |
| Bench current meter | Not applicable — omit power measurements, or (optional, clearly labeled as an estimate) instrument instruction/cycle counts per state as a proxy for relative power cost, explicitly caveated as "cycles, not measured current" in your README |

---

## 4. Cryptography

Unchanged: SHA-256 + ECDSA P-256 via micro-ecc or trimmed mbedTLS, `sign_image.py` host tool generating signed images offline. This is pure software and was never hardware-dependent in the first place — build and unit-test it exactly as in the original spec, entirely on your PC, before it ever touches the emulator.

---

## 5. Test plan (this is now easier and more thorough than the hardware version — lean into that)

### 5.1 Unit tests (host PC)
Same as before: metadata CRC/ping-pong selection, header validation, version comparison, all with mocked flash as memory buffers. Run in CI on every commit.

### 5.2 Functional tests under Renode
Drive these with a Robot Framework test suite (`.robot` files) or a Python script using Renode's `pyrenode3`/`Renode.Robot` bindings:
- Happy path: sign v1, load into Slot A, boot, confirm bootloader jumps in and the app calls `confirm_healthy`.
- OTA happy path: from v1 running, "deliver" v2 by writing the signed image directly into the emulated Slot B region (simulating what a real OTA transport would have written), update metadata, reset the machine, confirm it boots v2 and reaches CONFIRMED.
- Bad signature: flip a byte in a signed image, attempt the same flow, assert the bootloader rejects it and the metadata/active slot is unchanged.
- Rollback: build a "bad" v3 that never calls `confirm_healthy`; after MAX_TRIALS resets (scripted: reset the machine MAX_TRIALS+1 times in a loop, reading UART each time), assert the bootloader has reverted `active_slot` back to v2 and v2 is running.
- Anti-rollback: after v3 confirms and raises `min_allowed_version`, attempt to write v1 back into a slot and boot; assert rejection.

### 5.3 Fault injection — now exhaustive, not sampled
This is the part that gets meaningfully *better* in simulation. Instead of randomly sampling power-cut timings against a physical clock (as the hardware spec did with a relay), you can drive Renode to reset at **specific, chosen instruction counts or specific PC (program counter) addresses**, systematically walking through the entire update routine:

1. Set a breakpoint (or use Renode's instruction-count-based execution) at each meaningful point in the flash-write sequence: before erase, mid-erase, after erase before write, mid-write (per word/page if your flash model supports partial-write granularity), after write before metadata update, mid-metadata-write, after metadata-write before reset.
2. At each point, issue `machine Reset` (simulating power loss at exactly that instant) and then boot fresh.
3. Assert: the device always ends up running *some* valid, signature-verified image (old or new), and metadata is never left in a state where neither copy validates.
4. Because this is deterministic and scriptable, you can enumerate **every** critical instruction boundary in the update routine rather than hoping 500 random real-world power cuts happened to hit the dangerous windows. This is strictly stronger test coverage than the physical fault-injection rig, and you should say so explicitly in your results doc — it is a genuine advantage of the simulator-only approach, not just a workaround.
5. Log every (cut point → recovery outcome) pair to a CSV; your results table becomes "N distinct fault points tested, N recovered correctly" instead of "500 random cuts, 500 recovered."

### 5.4 Metrics to report (revised for no physical hardware)
- Boot time in emulated cycles (Renode can report simulated time and/or instruction counts), with vs without signature verification — still a meaningful relative number even without wall-clock hardware timing.
- SHA-256 + ECDSA verify time in cycles (or wall-clock if you also run these host-side for a sanity check, clearly labeled as host timing vs target timing).
- Fault-injection: number of distinct injection points tested, number recovered correctly (target: 100%).
- Bootloader/image binary sizes (real numbers, unaffected by simulation).
- Explicitly state in the README: "current/power draw was not measured; this project targets logic and fail-safety, not power characterization" — this is an honest, defensible scope statement, better than a fabricated number.

---

## 6. Build roadmap (phased, simulator-only)

**Phase 0 — Environment setup (0.5–1 day)**
- Install Renode, pick the exact STM32F4 platform description it ships with, confirm you can load and run a trivial "blink" (GPIO toggle, observed via Renode's GPIO/analyzer view or UART print) ELF on it.
- Generate the ECDSA keypair on your PC.

**Phase 1 — Bare bootloader, no crypto (2–3 days)**
- Linker scripts for bootloader and Slot A regions, matching the emulated flash layout.
- Bootloader reads a hardcoded slot, sets VTOR, jumps to it; get a trivial "blink" app running through the bootloader under Renode.
- Add image header with magic + CRC32 only; bootloader checks before jumping.

**Phase 2 — Dual slots + metadata (2–3 days)**
- Ping-pong metadata read/write with CRC validation.
- Bootloader picks active slot from metadata.
- Test: load different "blink" variants into Slot A/B, flip metadata via a Renode script poking memory directly, confirm the right one boots.

**Phase 3 — Crypto (3–4 days)**
- SHA-256 + ECDSA verify integration, `sign_image.py`.
- Bootloader rejects unsigned/tampered images, accepts signed ones — test with a deliberately bit-flipped image under Renode and confirm rejection.

**Phase 4 — Trial boot + rollback (3–4 days)**
- `confirm_healthy()` library, IWDG-based trial watchdog (Renode models this peripheral), automatic revert logic.
- Functional test 5.2 (rollback case) passes under scripted resets.

**Phase 5 — "OTA" delivery simulation + anti-rollback (2–3 days)**
- Simulate delivery by writing a signed image directly into the inactive slot's emulated flash region from a host-side script (representing "the transport layer already received and staged this image") — this is a legitimate simplification to state clearly in your design doc: you are testing the update *mechanism* (verification, atomicity, rollback), and treating the transport (UART/Wi-Fi bytes arriving) as a solved, out-of-scope problem, or implement a minimal UART receiver in the app and pipe bytes to it through Renode's UART if you want that piece covered too (optional but strengthens the project).
- Anti-rollback version ratchet, tested as in 5.2.

**Phase 6 — Exhaustive fault injection + CI + polish (3–4 days)**
- Build the Robot Framework/Python fault-injection harness from §5.3, enumerate injection points, run the full sweep, produce the results CSV and table.
- Wire everything into GitHub Actions: unit tests, and the Renode-based functional + fault-injection suite (Renode runs headlessly in CI, this is a first-class supported use case, not a hack).
- README with architecture diagram, results tables, and a design doc section explicitly stating the simulator-only scope and why the fault-coverage claim is actually stronger than a physical random-cut test.

Total: similar ~3-week cadence, arguably faster since there's no hardware setup/debugging friction, and the fault-injection phase is more thorough for less effort.

---

## 7. Repo layout

```
safeflash/
├── bootloader/
│   ├── src/            (main.c, flash_map.h, metadata.c, image_verify.c, boot.c)
│   ├── linker/          bootloader.ld
│   └── Makefile / CMakeLists.txt
├── app/
│   ├── src/             application code + confirm_healthy()
│   └── linker/          app_slot_a.ld, app_slot_b.ld
├── crypto/              micro-ecc or trimmed mbedTLS, vendored
├── tools/
│   └── sign_image.py
├── sim/
│   ├── platform.repl     (Renode platform description, if customized)
│   ├── boot.resc         (Renode script: load ELF, start machine)
│   └── robot/            (.robot test suites: functional + fault-injection)
├── tests/
│   └── unit/             host-run tests (metadata, header validation, version logic)
├── docs/
│   ├── design.md         (include an explicit "simulator-only scope" section)
│   └── results.md        (fault-injection sweep table, timing/cycle numbers)
└── .github/workflows/ci.yml   (unit tests + headless Renode functional/fault suite)
```

---

## 8. Open decisions to make before Phase 0

1. Exact STM32F4 part to target — pick whichever Renode's bundled platform files support most completely (check `renode/platforms/` for `.repl` files with flash + IWDG + UART modeled), rather than picking a part first and hoping Renode supports it.
2. micro-ecc + standalone SHA-256 vs trimmed mbedTLS — same tradeoff as before, unaffected by simulation.
3. Whether to implement a minimal in-app UART receiver for OTA bytes (more complete story) or treat transport as out-of-scope and inject the staged image directly (faster, still a legitimate and clearly-documented simplification, per Phase 5).
