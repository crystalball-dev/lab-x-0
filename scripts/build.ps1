# Configures and builds LAB X-0 (VST3, Standalone and the LabX0Render test harness).
param(
    [string]$Config = "Release",
    [string]$JucePath = "F:/_DEV/tools/JUCE"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake = "F:\_DEV\tools\cmake\cmake-4.4.4-windows-x86_64\bin\cmake.exe" }

& $cmake -S $root -B "$root\build" -G "Visual Studio 17 2022" -A x64 "-DLABX0_JUCE_PATH=$JucePath"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $cmake --build "$root\build" --config $Config --target LabX0_VST3 LabX0_Standalone LabX0Render --parallel
exit $LASTEXITCODE
