# Runs pluginval against the built VST3 bundle. Strictness 5 is the minimum for host compatibility; 10 adds fuzzing.
param([int]$Strictness = 5)

$root = Split-Path -Parent $PSScriptRoot
$bundle = "$root\build\LabX0_artefacts\Release\VST3\LAB X-0.vst3"

$pluginval = (Get-Command pluginval -ErrorAction SilentlyContinue).Source
if (-not $pluginval) { $pluginval = "F:\_DEV\tools\pluginval\pluginval.exe" }

if (-not (Test-Path $bundle)) {
    Write-Error "Build the plugin first: $bundle not found"
    exit 1
}

$log = "$root\build\pluginval-level$Strictness.log"
& $pluginval --strictness-level $Strictness --validate $bundle | Out-File -FilePath $log -Encoding utf8
$code = $LASTEXITCODE
Get-Content $log -Tail 5
Write-Output "pluginval exit code: $code (log: $log)"
exit $code
