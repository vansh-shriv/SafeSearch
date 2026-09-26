# Run the Robot Framework suites under Renode's test runner.
# Usage: powershell -File sim/run_robot.ps1 [-Suite signature|trial|ota|powercut|smoke ...]
# Needs `mingw32-make` first (firmware in build/). Results: build/robot/{log,report}.html
#
# One-time setup: py -3 -m pip install robotframework==6.1 robotframework-retryfailed==0.2.0 psutil pyyaml telnetlib3
param([string[]]$Suite = @("signature", "trial", "ota", "powercut"))

$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
try {
    # Robot 6.1 cannot handle the default 'utf-8:surrogateescape' console encoding under newer Pythons.
    $env:PYTHONIOENCODING = "utf-8"
    python tests/renode/gen_artifacts.py
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    # `-File` invocations pass "a,b" as one string, so split explicitly and force an array for splatting.
    $files = @(($Suite -join ",") -split "," | Where-Object { $_ } | ForEach-Object { "sim/robot/$_.robot" })
    & 'C:\Program Files\Renode\bin\renode-test.bat' @files -r build/robot
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
