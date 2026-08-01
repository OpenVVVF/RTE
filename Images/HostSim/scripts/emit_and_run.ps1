# Run RTECodeEmitter on a graph, then build the emitted HostSim tree.
# Usage:  powershell -File scripts\emit_and_run.ps1 [-Graph <path>]
param(
    [string]$Graph = ""
)

$ErrorActionPreference = "Stop"

$hostSimRoot = Split-Path $PSScriptRoot -Parent
$repoRoot = Split-Path (Split-Path $hostSimRoot -Parent) -Parent
if ($Graph -eq "") { $Graph = Join-Path $hostSimRoot "baseline_graph.json" }

$emitter = Join-Path $repoRoot "build\Source\RTECodeEmitter\RTECodeEmitter.exe"

Get-Process | Where-Object { $_.ProcessName -match '^(host_sim|NodeGUI)$' } |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

$winEmitted = Join-Path $repoRoot "build\hostsim_emitted"
if (Test-Path $winEmitted) {
    Remove-Item -LiteralPath $winEmitted -Recurse -Force -ErrorAction SilentlyContinue
}

& $emitter --base-src (Join-Path $repoRoot "Images\HostSim") --graph $Graph --output $winEmitted --verbosity info
if ($LASTEXITCODE -ne 0) { throw "RTECodeEmitter failed" }

$emitted = Join-Path $repoRoot "build\hostsim_emitted"
$buildDir = Join-Path $repoRoot "build\hostsim_emitted_build"
Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue

cmake -S $emitted -B $buildDir
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

$exe = Join-Path $buildDir "host_sim.exe"
if (-not (Test-Path $exe)) { $exe = Join-Path $buildDir "host_sim" }
if (-not (Test-Path $exe)) { $exe = Join-Path $buildDir "Debug\host_sim.exe" }
if (-not (Test-Path $exe)) { $exe = Join-Path $buildDir "Release\host_sim.exe" }

$scenario = Join-Path $emitted "scenarios\default_motor.json"
& $exe $scenario
Write-Host "Emit-and-run complete: $exe"
