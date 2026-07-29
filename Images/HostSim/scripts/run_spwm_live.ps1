# Emit the SPWM demo graph, build HostSim, and launch live + NodeGUI.
param(
    [switch]$NoGui,
    [switch]$ForceEmit
)

$ErrorActionPreference = "Stop"

$hostSimRoot = Split-Path $PSScriptRoot -Parent
$repoRoot = Split-Path (Split-Path $hostSimRoot -Parent) -Parent
$graph = Join-Path $hostSimRoot "graphs\spwm_demo_graph.json"
$scenario = Join-Path $hostSimRoot "scenarios\spwm_demo.json"

function To-WslPath([string]$p) {
    $full = (Resolve-Path $p).Path
    return "/mnt/" + $full.Substring(0, 1).ToLower() + ($full.Substring(2) -replace '\\', '/')
}

$wslRepo = To-WslPath $repoRoot
$wslGraph = To-WslPath $graph
$emitter = "/opt/rtehost/build/Source/RTECodeEmitter/RTECodeEmitter"

function Stop-SimApps {
    Get-Process | Where-Object { $_.ProcessName -match '^(host_sim|NodeGUI)$' } |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
}

function Clear-EmittedTree([string]$repoRoot, [string]$relativePath) {
    $winPath = Join-Path $repoRoot $relativePath
    if (Test-Path $winPath) {
        Remove-Item -LiteralPath $winPath -Recurse -Force -ErrorAction SilentlyContinue
    }
    $wslRepo = To-WslPath $repoRoot
    wsl -d Ubuntu -u root -- bash -lc "cd $wslRepo && rm -rf $relativePath" 2>$null
}

$emittedRel = "build/hostsim_spwm_emitted"
$buildDir = Join-Path $repoRoot "build\hostsim_spwm_emitted_build"

Write-Host "Stopping running HostSim / NodeGUI (unlocks emit output)..."
Stop-SimApps

$emitted = Join-Path $repoRoot "build\hostsim_spwm_emitted"
$exe = Join-Path $buildDir "Debug\host_sim.exe"
if (-not (Test-Path $exe)) { $exe = Join-Path $buildDir "host_sim.exe" }

$needEmit = $ForceEmit.IsPresent -or -not (Test-Path $exe)
if (-not $needEmit -and (Test-Path $exe)) {
    $graphTime = (Get-Item $graph).LastWriteTimeUtc
    $exeTime = (Get-Item $exe).LastWriteTimeUtc
    if ($graphTime -gt $exeTime) {
        Write-Host "Graph newer than emitted build - re-emitting..."
        $needEmit = $true
    }
}

if ($needEmit) {
    Clear-EmittedTree $repoRoot $emittedRel
    Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue

    Write-Host "Emitting SPWM graph..."
    $emitCmd = "cd $wslRepo && ${emitter} --base-src Images/HostSim --graph $wslGraph --output $emittedRel --verbosity info"
    wsl -d Ubuntu -u root -- bash -lc $emitCmd
    if ($LASTEXITCODE -ne 0) { throw "RTECodeEmitter failed" }

    cmake -S $emitted -B $buildDir
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
    cmake --build $buildDir --config Debug
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

    $exe = Join-Path $buildDir "Debug\host_sim.exe"
    if (-not (Test-Path $exe)) { $exe = Join-Path $buildDir "host_sim.exe" }
} else {
    Write-Host "Using existing emitted SPWM build (pass -ForceEmit to rebuild)."
}

if (-not (Test-Path $exe)) { throw "host_sim.exe not found in $buildDir" }

Write-Host "Starting HostSim SPWM live..."
Start-Process -FilePath $exe -ArgumentList $scenario, "--live", "--realtime", "1.0" -WorkingDirectory $emitted

if (-not $NoGui) {
    Start-Sleep -Seconds 1
    $guiWd = Join-Path $repoRoot "build\Source\NodeGUI"
    $graphArg = (Resolve-Path $graph).Path
    Start-Process -FilePath (Join-Path $guiWd "NodeGUI.exe") `
        -ArgumentList $graphArg, "--tcp", "127.0.0.1:14608", "--protocol", "ivp" `
        -WorkingDirectory $guiWd
    Write-Host "NodeGUI opened with SPWM graph + live telemetry."
}

Write-Host ""
Write-Host "Live controls:"
Write-Host "  Throttle A -> modulation index (0..1)"
Write-Host "  Throttle B -> electrical frequency map (1..20 Hz)"
Write-Host "Plot: duty_u/v/w (slow), pwm_gate_u/v/w + pwm_v_uv (scope), i_a/b/c"
