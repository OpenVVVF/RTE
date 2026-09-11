# Emit a graph into HostSim, build it, and launch live + RTEStudio.
# Defaults to the SPWM demo graph for backward compatibility with the demo menu.
# Mirrors run_spwm_live.sh: RTE_EMITTER / RTE_GUI override the tool paths.
param(
    [switch]$NoGui,
    [switch]$ForceEmit,
    [string]$Graph = "",
    [string]$Scenario = ""
)

$ErrorActionPreference = "Stop"

$hostSimRoot = Split-Path $PSScriptRoot -Parent
$repoRoot = Split-Path (Split-Path $hostSimRoot -Parent) -Parent

if ([string]::IsNullOrEmpty($Graph)) {
    $Graph = Join-Path $hostSimRoot "graphs\spwm_demo_graph.json"
}
if (-not (Test-Path $Graph)) { throw "Graph not found: $Graph" }
$graph = (Resolve-Path $Graph).Path

$graphName = [System.IO.Path]::GetFileNameWithoutExtension($graph)

if ([string]::IsNullOrEmpty($Scenario)) {
    # Prefer a scenario named after the graph (minus a trailing _graph),
    # else the generic motor scenario — same rule as `rte sim`.
    $scenarioBase = $graphName
    if ($scenarioBase.EndsWith("_graph")) {
        $scenarioBase = $scenarioBase.Substring(0, $scenarioBase.Length - 6)
    }
    $candidate = Join-Path $hostSimRoot "scenarios\${scenarioBase}.json"
    if (Test-Path $candidate) {
        $Scenario = $candidate
    } else {
        $Scenario = Join-Path $hostSimRoot "scenarios\default_motor.json"
    }
}
if (-not (Test-Path $Scenario)) { throw "Scenario not found: $Scenario" }
$scenario = (Resolve-Path $Scenario).Path

$emitter = if ($env:RTE_EMITTER) { $env:RTE_EMITTER } else { Join-Path $repoRoot "build\bin\RTECodeEmitter.exe" }
$rteGui  = if ($env:RTE_GUI)     { $env:RTE_GUI }     else { Join-Path $repoRoot "build\bin\RTEStudio.exe" }

if (-not (Test-Path $emitter)) {
    throw "RTECodeEmitter not found at $emitter (set RTE_EMITTER; build host tools: cmake -B build && cmake --build build --target RTECodeEmitter)"
}

function Stop-SimApps {
    # Exact names only: never kill the powershell host or an unrelated RTEStudio
    # instance launched by the IDE.
    Get-Process | Where-Object { $_.ProcessName -match '^(host_sim|RTEStudio)$' } |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
}

$emittedRel = "build\hostsim_${graphName}_emitted"
$emitted = Join-Path $repoRoot $emittedRel
$buildDir = Join-Path $repoRoot "${emittedRel}_build"

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

Write-Host "Stopping running HostSim / RTEStudio (unlocks emit output and the 14608 port)..."
Stop-SimApps

if ($needEmit) {
    if (Test-Path $emitted) { Remove-Item -LiteralPath $emitted -Recurse -Force }
    if (Test-Path $buildDir) { Remove-Item -LiteralPath $buildDir -Recurse -Force }

    Write-Host "Emitting ${graphName} graph into HostSim..."
    & $emitter --base-src $hostSimRoot --graph $graph --output $emitted --verbosity info
    if ($LASTEXITCODE -ne 0) { throw "RTECodeEmitter failed" }

    cmake -S $emitted -B $buildDir
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
    cmake --build $buildDir --config Debug
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

    $exe = Join-Path $buildDir "Debug\host_sim.exe"
    if (-not (Test-Path $exe)) { $exe = Join-Path $buildDir "host_sim.exe" }
} else {
    Write-Host "Using existing emitted ${graphName} build (pass -ForceEmit to rebuild)."
}

if (-not (Test-Path $exe)) { throw "host_sim.exe not found in $buildDir" }

Write-Host "Starting HostSim live for ${graphName}..."
Start-Process -FilePath $exe -ArgumentList $scenario, "--live", "--realtime", "1.0" -WorkingDirectory $emitted

if (-not $NoGui) {
    Start-Sleep -Seconds 1
    if (Test-Path $rteGui) {
        Start-Process -FilePath $rteGui `
            -ArgumentList $graph, "--tcp", "127.0.0.1:14608", "--protocol", "ivp" `
            -WorkingDirectory $repoRoot
        Write-Host "RTEStudio opened with ${graphName} graph + live telemetry."
    } else {
        Write-Host "RTEStudio not found at $rteGui (set RTE_GUI; build with: cmake --build build --target RTEStudio)"
        Write-Host "HostSim is still running."
    }
}

Write-Host ""
Write-Host "Scenario: $scenario"
Write-Host "Live telemetry: 127.0.0.1:14608 (IVP)"
