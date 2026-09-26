*** Settings ***
Library           OperatingSystem
Resource          ${RENODEKEYWORDS}
Variables         ${CURDIR}${/}..${/}..${/}build${/}robot_art${/}vars.py

*** Variables ***
${ROOT}           ${CURDIR}${/}..${/}..
${ART}            build/robot_art
${META_ADDR}      0x08008000
${STAGE_ADDR}     0x20020000
${T}              40

*** Keywords ***
Boot Machine
    [Documentation]    Create the emulated STM32F4 with bootloader + slot images, optionally stage device
    ...                metadata, an OTA blob in RAM, and extra monitor commands, then start it.
    [Arguments]    ${slotA}=build/app_v1_slotA.img    ${slotB}=build/app_v2_slotB.img
    ...            ${meta}=${EMPTY}    ${stage}=${EMPTY}    ${extra}=${EMPTY}
    ${root}=    Normalize Path    ${ROOT}
    Execute Command    path add "${root}"
    Execute Command    $slotA=@${slotA}
    Execute Command    $slotB=@${slotB}
    Execute Script     ${root}/sim/boot.resc
    Run Keyword If     '${meta}' != ''     Execute Command    sysbus LoadBinary @${meta} ${META_ADDR}
    Run Keyword If     '${stage}' != ''    Execute Command    sysbus LoadBinary @${stage} ${STAGE_ADDR}
    Run Keyword If     '${extra}' != ''    Execute Command    ${extra}
    Create Terminal Tester    sysbus.usart1
    Start Emulation

Uart Should Show
    [Arguments]    ${text}    ${timeout}=${T}
    Wait For Line On Uart    ${text}    timeout=${timeout}

Uart Should Not Show
    [Arguments]    ${text}    ${timeout}=4
    Should Not Be On Uart    ${text}    timeout=${timeout}

Power Cycle
    [Documentation]    Reset the whole machine (flash persists). Optionally stage an OTA blob in RAM while the
    ...                machine is paused, so it is in place before the bootloader/app run.
    [Arguments]    ${stage}=${EMPTY}
    Execute Command    pause
    Execute Command    machine Reset
    Run Keyword If     '${stage}' != ''    Execute Command    sysbus LoadBinary @${stage} ${STAGE_ADDR}
    Execute Command    start
