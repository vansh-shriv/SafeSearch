*** Settings ***
Documentation     Attack images with all unkeyed CRCs recomputed, so only SHA-256 / ECDSA can reject them.
...               Slot A gets the attack image, slot B the genuine v2: the bootloader must name the right
...               reason for slot A and fall back to v2.
Suite Setup       Setup
Suite Teardown    Teardown
Test Setup        Reset Emulation
Test Teardown     Test Teardown
Resource          safeflash.resource
Test Template     Attack Image Is Rejected

*** Test Cases ***                        IMAGE                          REASON
Stale payload hash                        attack_stale_hash.img          bad payload hash
Stale signature                           attack_stale_signature.img     bad signature
Version bumped with old signature         attack_version_bump.img        bad signature
Signed with attacker key                  attack_wrong_key.img           bad signature
Zeroed signature                          attack_zero_signature.img      bad signature

Genuine image boots
    [Template]    NONE
    Boot Machine
    Uart Should Show    BL: slot A: ok
    Uart Should Show    APP: running, version 1

*** Keywords ***
Attack Image Is Rejected
    [Arguments]    ${image}    ${reason}
    Boot Machine    slotA=${ART}/${image}
    Uart Should Show    BL: slot A: ${reason}
    Uart Should Show    BL: slot B: ok
    Uart Should Show    BL: reverted to slot B
    Uart Should Show    APP: running, version 2
