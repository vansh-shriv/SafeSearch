# Headless ad-hoc run: boot the machine for N simulated seconds and print USART1 output.
# Usage: powershell -File sim/run.ps1 [-Seconds 0.5]
# The UART log goes to %TEMP%\safeflash (Renode's @path syntax cannot handle this repo's spaces).
param([double]$Seconds = 0.5, [string]$Steps = "", [string]$SlotA = "", [string]$SlotB = "", [string]$Pre = "")

$repo = Split-Path -Parent $PSScriptRoot
$logdir = Join-Path $env:TEMP "safeflash"
New-Item -ItemType Directory -Force $logdir | Out-Null
$log = Join-Path $logdir "uart.log"
Remove-Item $log -ErrorAction SilentlyContinue
$logfwd = $log -replace '\\', '/'

# Optional image overrides (repo-relative, no spaces), applied before boot.resc's defaults.
$overrides = ""
if ($SlotA) { $overrides += "`$slotA=@$SlotA; " }
if ($SlotB) { $overrides += "`$slotB=@$SlotB; " }

Push-Location $repo
try {
    & 'C:\Program Files\Renode\bin\Renode.exe' --console --disable-xwt --plain `
        -e "$($overrides)include @sim/boot.resc; sysbus.usart1 CreateFileBackend @$logfwd true; $Pre emulation RunFor '$Seconds'; $Steps quit" | Out-Null
} finally {
    Pop-Location
}
Get-Content $log -ErrorAction SilentlyContinue
