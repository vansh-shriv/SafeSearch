*** Settings ***
Documentation     Simulated OTA (signed image staged in RAM, installed by the running app), the anti-rollback
...               ratchet, and rejection of a delivered image with a bad signature.
Suite Setup       Setup
Suite Teardown    Teardown
Test Setup        Reset Emulation
Test Teardown     Test Teardown
Resource          safeflash.resource

*** Test Cases ***
OTA v1 to v2 installs, trials, confirms and ratchets the floor
    Boot Machine    stage=${ART}/stage_v2.bin
    Uart Should Show    APP: installing update
    Uart Should Show    APP: install ok, resetting
    Uart Should Show    active B state 1 trials 0
    Uart Should Show    BL: trial 1/3, watchdog armed
    Uart Should Show    APP: running, version 2
    Uart Should Show    APP: confirmed healthy
    Power Cycle
    Uart Should Show    state 2 trials 0 floor 0x00000002

Installer refuses a validly signed older image
    Boot Machine    stage=${ART}/stage_v2.bin
    Uart Should Show    APP: install ok, resetting
    Uart Should Show    APP: running, version 2
    Uart Should Show    APP: confirmed healthy
    Power Cycle
    Uart Should Show    state 2 trials 0 floor 0x00000002
    Uart Should Show    APP: running, version 2
    # v2 (floor 2) is confirmed; now deliver the genuine, validly signed v1
    Power Cycle    stage=${ART}/stage_v1.bin
    Uart Should Show    APP: install failed rc 0xFFFFFFFD
    Uart Should Not Show    SafeFlash BL    timeout=3

Bootloader enforces the floor even if metadata points at an old image
    Boot Machine    meta=${ART}/meta_floor2_trial_a.bin
    Uart Should Show    BL: slot A: below version floor
    Uart Should Show    APP: running, version 2

Delivered image with a bad signature is installed but rejected at boot
    Boot Machine    stage=${ART}/stage_evil.bin
    Uart Should Show    APP: install ok, resetting
    Uart Should Show    BL: slot B: bad signature
    Uart Should Show    BL: reverted to slot A
    Uart Should Show    APP: running, version 1
    Uart Should Not Show    APP: running, version 2    timeout=3
