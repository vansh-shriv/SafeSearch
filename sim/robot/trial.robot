*** Settings ***
Documentation     Trial boot, confirm, IWDG watchdog and automatic revert. Metadata is staged as "slot B holds a
...               freshly installed image in TRIAL, slot A is the confirmed old image".
Suite Setup       Setup
Suite Teardown    Teardown
Test Setup        Reset Emulation
Test Teardown     Test Teardown
Resource          safeflash.resource

*** Test Cases ***
Good image confirms itself and survives past the watchdog window
    Boot Machine    meta=${ART}/meta_trial_b.bin
    Uart Should Show    BL: trial 1/3, watchdog armed
    Uart Should Show    APP: running, version 2
    Uart Should Show    APP: confirmed healthy
    # the trial window is 2 s; 5 emulated seconds with no reboot means the confirmed image is being kicked
    Uart Should Not Show    SafeFlash BL    timeout=5

Confirmed image boots normally next time
    Boot Machine    meta=${ART}/meta_trial_b.bin
    Uart Should Show    APP: confirmed healthy
    Power Cycle
    Uart Should Show    state 2 trials 0 floor 0x00000002
    Uart Should Not Show    watchdog armed    timeout=2

Image that never confirms is reverted after MAX_TRIALS
    Boot Machine    slotB=build/app_v3_slotB_bad.img    meta=${ART}/meta_trial_b.bin
    Uart Should Show    BL: trial 1/3, watchdog armed
    Uart Should Show    APP: running, version 3
    Uart Should Show    BL: trial 2/3, watchdog armed
    Uart Should Show    BL: trial 3/3, watchdog armed
    Uart Should Show    BL: trial limit reached
    Uart Should Show    BL: reverted to slot A
    Uart Should Show    APP: running, version 1
    Uart Should Show    APP: confirmed healthy
    # after the revert there are no more watchdog resets
    Uart Should Not Show    SafeFlash BL    timeout=5
