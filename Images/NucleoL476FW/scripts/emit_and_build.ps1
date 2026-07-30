# Run RTECodeEmitter (in WSL, where the host tools build) on a graph, then
# rebuild the emitted firmware tree for ARM on Windows.
# Usage:  powershell -File scripts\emit_and_build.ps1 [-Graph <path>] [-Flash]
param(
    [string]$Graph = "",
    [switch]$Flash
)

$ErrorActionPreference = "Stop"

$fwRoot = Split-Path $PSScriptRoot -Parent
$repoRoot = Split-Path (Split-Path $fwRoot -Parent) -Parent
if ($Graph -eq "") { $Graph = Join-Path $fwRoot "baseline_graph.json" }

function To-WslPath([string]$p) {
    $full = (Resolve-Path $p).Path
    return "/mnt/" + $full.Substring(0, 1).ToLower() + ($full.Substring(2) -replace '\\', '/')
}

$wslRepo = To-WslPath $repoRoot
$wslGraph = To-WslPath $Graph

# RTECodeEmitter host binary: built once via the /opt/rtehost superbuild
# (symlinks Lib/ and Source/ from this repo; see README "Host tools" section).
$emitter = "/opt/rtehost/build/Source/RTECodeEmitter/RTECodeEmitter"

wsl -d Ubuntu -u root -- bash -lc "cd $wslRepo && chmod -R 777 build/nucleo_emitted 2>/dev/null; rm -rf build/nucleo_emitted 2>/dev/null; $emitter --base-src Images/NucleoL476FW --graph $wslGraph --output build/nucleo_emitted --verbosity info"
if ($LASTEXITCODE -ne 0) { throw "RTECodeEmitter failed" }

$emitted = Join-Path $repoRoot "build\nucleo_emitted"
$buildDir = Join-Path $repoRoot "build\nucleo_emitted_build"
Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue

& "$PSScriptRoot\build.ps1" -Source $emitted -BuildDir $buildDir

if ($Flash) {
    & "$PSScriptRoot\flash.ps1" -Elf (Join-Path $buildDir "STM32CubeMX.elf")
}
