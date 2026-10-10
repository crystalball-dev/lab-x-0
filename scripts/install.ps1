# Installs the built plugin into the standard VST3 folder as a single .vst3 file.
# FL Studio 20 reliably scans single-file VST3s; never keep a bundle folder of the same name next to it.
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$source = "$root\build\LabX0_artefacts\Release\VST3\LAB X-0.vst3\Contents\x86_64-win\LAB X-0.vst3"
$dest = "C:\Program Files\Common Files\VST3\LAB X-0.vst3"

if (-not (Test-Path $source -PathType Leaf)) {
    Write-Error "Build the plugin first: $source not found"
    exit 1
}
if (Test-Path $dest -PathType Container) {
    Write-Error "A bundle folder already exists at $dest. Remove it so FL does not list LAB X-0 twice."
    exit 1
}
if (Test-Path $dest -PathType Leaf) {
    # A host that has loaded the plugin keeps the file open. Replacing it then would fail half way.
    try {
        $stream = [System.IO.File]::Open($dest, 'Open', 'ReadWrite', 'None')
        $stream.Close()
    } catch {
        Write-Error "LAB X-0 is in use (FL Studio has it loaded). Close FL Studio and run this again."
        exit 1
    }
}

Copy-Item $source $dest -Force
$item = Get-Item $dest
Write-Output ("Installed {0} ({1:N0} bytes, {2})" -f $item.FullName, $item.Length, $item.LastWriteTime)
