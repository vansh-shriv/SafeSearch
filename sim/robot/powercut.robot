*** Settings ***
Documentation     Power cuts on the real firmware binary. The running v1 app installs a staged v2. A hook on the
...               flash driver's word-program / sector-erase routine freezes the CPU when it reaches the chosen
...               address (a frozen CPU touches nothing, so flash is exactly as a power cut would leave it),
...               then the machine is reset. The bootloader must recover and boot the old v1; the update must
...               not have been applied. Complements the exhaustive host sweep (tests/unit/fault_sweep.c).
Suite Setup       Setup
Suite Teardown    Teardown
Test Setup        Reset Emulation
Test Teardown     Test Teardown
Resource          safeflash.resource
Test Template     Cut Power At

*** Variables ***
${SLOT_B}         ${{ 0x08080000 }}
${META_A}         ${{ 0x08008000 }}
${PAYLOAD_B}      ${{ 0x08080400 }}

*** Test Cases ***                                  FUNCTION            VALUE
Before erasing the target slot (sector 8)           ${FLASH_ERASE}      8
Payload first word                                  ${PROGRAM_WORD}     ${PAYLOAD_B}
Payload middle word                                 ${PROGRAM_WORD}     ${{ ${PAYLOAD_B} + 4 * (${PAYLOAD_WORDS} // 2) }}
Payload last word                                   ${PROGRAM_WORD}     ${{ ${PAYLOAD_B} + 4 * (${PAYLOAD_WORDS} - 1) }}
Header word 0 (first)                               ${PROGRAM_WORD}     ${SLOT_B}
Header word 14                                      ${PROGRAM_WORD}     ${{ ${SLOT_B} + 4 * 14 }}
Header word 28 (last with data)                     ${PROGRAM_WORD}     ${{ ${SLOT_B} + 4 * 28 }}
Header word 255 (last padding word)                 ${PROGRAM_WORD}     ${{ ${SLOT_B} + 4 * 255 }}
Before erasing the metadata sector (sector 2)       ${FLASH_ERASE}      2
Metadata commit word 0                              ${PROGRAM_WORD}     ${META_A}
Metadata commit word 1                              ${PROGRAM_WORD}     ${{ ${META_A} + 4 }}
Metadata commit word 2                              ${PROGRAM_WORD}     ${{ ${META_A} + 8 }}
Metadata commit word 3                              ${PROGRAM_WORD}     ${{ ${META_A} + 12 }}
Metadata commit word 4 (CRC)                        ${PROGRAM_WORD}     ${{ ${META_A} + 16 }}

Control: no power cut ends on v2
    [Template]    NONE
    Boot Machine    stage=${ART}/stage_v2.bin
    Uart Should Show    APP: install ok, resetting
    Uart Should Show    APP: running, version 2
    Uart Should Show    APP: confirmed healthy

*** Keywords ***
Cut Power At
    [Arguments]    ${function}    ${value}
    ${hook}=    Set Variable    sysbus.cpu AddHook ${function} "if self.GetRegister(0).RawValue == ${value}: self.PC = type(self.PC).Create(${SPIN_PC}, 32)"
    Boot Machine    stage=${ART}/stage_v2.bin    extra=${hook}
    Uart Should Show    APP: installing update
    # CPU is now frozen mid-install: nothing further may print, in particular no successful install
    Uart Should Not Show    APP: install ok    timeout=2
    Power Cycle
    Uart Should Show    APP: running, version 1
    Uart Should Not Show    APP: running, version 2    timeout=4
