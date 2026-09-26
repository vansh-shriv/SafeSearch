# SafeFlash design

A secure bootloader with signed A/B updates for an STM32F4 (Cortex-M4), developed and tested entirely in the
Renode emulator plus a host-side fault-injection harness.

## Simulator-only scope

There is no physical board in this project. Everything runs in Renode (STM32F4 platform description, 2 MB
flash, IWDG, USART1) or on the host. What that does and does not establish:

- **Established:** the boot, verification, trial, revert and anti-rollback logic; the update ordering and
  metadata atomicity; behaviour under power loss at every flash mutation the update, confirm and boot paths
  perform. The firmware binaries are the same ones that would run on hardware.
- **Not established:** anything about real flash timing or electrical behaviour. Renode's flash controller
  ignores the programming-size field and the SR error bits, and programs a 32-bit word atomically, so real
  partial-word programming and flash error handling are not exercised in the emulator.
- **Not measured:** current/power draw. This project targets logic and fail-safety, not power
  characterisation, and reports no power number.
- **OTA transport is simulated.** Delivery is modelled by the host placing a complete signed image in a RAM
  staging area that the app then installs. The update *mechanism* (verification, atomicity, rollback) is under
  test; the byte transport (UART/Wi-Fi) is out of scope.
- **Metadata is CRC-protected, not authenticated.** An attacker who can write flash directly can forge it,
  including the anti-rollback floor. The threat model is image authenticity, not flash-bus tampering.

Why the fault coverage claim is strong even without hardware: a physical power-cut rig samples random
instants and can only hope to hit the dangerous windows. The host sweep instead enumerates *every* flash
mutation and every byte boundary inside each write (plus three torn-erase states), so the coverage is
systematic rather than sampled. See `docs/results.md` for the numbers and for how the harness itself was
validated.

## Flash map (from Renode's `stm32f4.repl`)

| Region | Address | Size |
|---|---|---|
| Bootloader (sectors 0-1) | `0x08000000` | 32 KB |
| Metadata copy A (sector 2) | `0x08008000` | 16 KB |
| Metadata copy B (sector 3) | `0x0800C000` | 16 KB |
| Unused (sector 4) | `0x08010000` | 64 KB |
| Slot A (sectors 5-7) | `0x08020000` | 384 KB |
| Slot B (sectors 8-10) | `0x08080000` | 384 KB |

Each slot starts with a `0x400`-byte header region; the vector table follows at slot + `0x400`. Apps are
position-dependent, so each slot has its own linker script.

## Structures

`image_header_t` (`bootloader/src/image_verify.h`): magic `0x53414645`, version, image size, payload CRC-32,
payload SHA-256, 64-byte ECDSA signature (raw r||s), header CRC-32.

`boot_metadata_t` (`bootloader/src/metadata.h`): magic, sequence number, active slot, boot state, trial
count, `min_allowed_version` (anti-rollback floor), CRC-32. Two copies (sectors 2 and 3) are written
alternately; the newest valid copy wins.

## Trust model

The signature is ECDSA P-256 over SHA-256 of the first 48 header bytes: magic, version, image size, CRC-32
and the payload SHA-256. So the version and size cannot be altered without invalidating the signature, and
the payload is bound through its hash. CRCs are unkeyed and only catch corruption; the Renode attack tests
recompute all CRCs so that only the cryptography can reject a tampered image. The public key is baked into the
bootloader from `build/pubkey.c`, generated from a gitignored dev private key.

## Boot flow (`bootloader/src/boot_logic.c`)

1. Load the newest valid metadata; if none, initialise (slot A, NORMAL, floor 0).
2. If the state is TRIAL and `trial_count >= MAX_TRIALS`: revert. Otherwise verify the active slot: structural
   checks (magic, header CRC, size, version floor, payload CRC), then payload hash and signature.
3. On revert or a failed active slot: verify the other slot (it must also satisfy the floor); commit
   metadata (active = other, CONFIRMED, count 0). If it fails too, halt.
4. If the resulting state is TRIAL: increment `trial_count` and commit it *before* jumping, so a hang or crash
   is charged to the attempt. Arm the IWDG (`TRIAL_WDT_MS`), then jump.

## Trial, confirm and rollback

`sf_confirm_healthy()` (app library) marks the running image CONFIRMED and ratchets the floor to its version in
one metadata commit. In a TRIAL boot `sf_wdt_kick()` refuses to kick the watchdog until confirmation
succeeded, so an image that runs but never confirms is still reset when the window ends. That makes the
trial bounded, not just hang detection. After `MAX_TRIALS` failed attempts the bootloader reverts. If the
confirm write is interrupted the state stays TRIAL, so the worst case is a revert, never an unsafe state.

## Update path (`app/src/safeflash_update.c`)

`sf_install_update()` refuses images below the floor, erases and programs the *inactive* slot (payload first,
header last), and only then commits metadata pointing at it in TRIAL. The running slot is never modified.
The installer does not verify the signature; the bootloader does, and reverts on failure. A torn or forged
image therefore cannot boot.

## Fault model used by the host sweep

For every flash mutation an operation performs: a write is cut after each byte count `0..len-1`; an erase is
cut in three torn states (never took effect, sector left as garbage, half erased). After the cut the device
boots and invariants are asserted (see `docs/results.md`). Not modelled: a second power cut during the
recovery boot (its own metadata writes are covered individually), and real word-granular flash behaviour.
