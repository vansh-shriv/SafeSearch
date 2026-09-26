*** Settings ***
Documentation     Serial recovery mode end to end: the real bootloader in Renode, USART1 exposed as a TCP socket, and
...               tools/recover.py as the host. Slots are made unbootable with attack images, or recovery is
...               requested through the RAM request word.
Suite Setup       Setup
Suite Teardown    Teardown
Test Setup        Reset Emulation
Test Teardown     Test Teardown
Resource          safeflash.resource

*** Variables ***
${BAD}            ${ART}/attack_zero_signature.img

*** Test Cases ***
Bootloader enters recovery when nothing is bootable, then installs a signed image
    Boot Machine    slotA=${BAD}    slotB=${BAD}    serial_port=3456
    Uart Should Show    BL: slot A: bad signature
    Uart Should Show    BL: slot B: bad signature
    Uart Should Show    BL: no bootable image, halting
    Uart Should Show    BL: recovery mode, waiting for image
    ${res}=    Send Image Over Serial    build/app_v2_slotB.img    3456
    Should Be Equal As Integers    ${res.rc}    0
    Uart Should Show    BL: recovery image installed, resetting
    Uart Should Show    BL: trial 1/3, watchdog armed
    Uart Should Show    APP: running, version 2
    Uart Should Show    APP: confirmed healthy

A forged image is refused over serial and the device keeps waiting
    Boot Machine    slotA=${BAD}    slotB=${BAD}    serial_port=3457
    Uart Should Show    BL: recovery mode, waiting for image
    # payload changed with all CRCs consistent, so only the signature/hash check can refuse it
    ${res}=    Send Image Over Serial    ${ART}/attack_stale_signature.img    3457
    Should Be Equal As Integers    ${res.rc}    1
    Should Contain    ${res.stderr}    bad image
    Uart Should Show    BL: recovery image rejected
    Uart Should Not Show    APP: running    timeout=3
    # the genuine image still installs afterwards
    ${res}=    Send Image Over Serial    build/app_v2_slotB.img    3457
    Should Be Equal As Integers    ${res.rc}    0
    Uart Should Show    APP: running, version 2

Recovery can be requested on a healthy device and installs into the inactive slot
    Boot Machine    serial_port=3458
    Uart Should Show    APP: running, version 1
    Power Cycle    request=${ART}/recovery_request.bin
    Uart Should Show    BL: recovery requested
    Uart Should Show    BL: recovery mode, waiting for image
    ${res}=    Send Image Over Serial    build/app_v2_slotB.img    3458
    Should Be Equal As Integers    ${res.rc}    0
    Uart Should Show    BL: recovery image installed, resetting
    Uart Should Show    active B state 1 trials 0
    Uart Should Show    APP: running, version 2

The recovery request is consumed: the next reset boots normally
    Boot Machine    serial_port=3459
    Uart Should Show    APP: running, version 1
    Power Cycle    request=${ART}/recovery_request.bin
    Uart Should Show    BL: recovery mode, waiting for image
    Power Cycle
    Uart Should Show    APP: running, version 1
    Uart Should Not Show    BL: recovery mode    timeout=3
