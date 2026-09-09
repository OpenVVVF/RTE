# Emit a graph into HostSim, build it, and launch live + NodeGUI.
# Defaults to the SPWM demo graph for backward compatibility with the demo menu.
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

$graphName = [System.IO.Path]::GetFileNameWithoutExtension($Graph)

if ([string]::IsNullOrEmpty($Scenario)) {
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

$graph = $Graph
$scenario = $Scenario

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

$emittedRel = "build/hostsim_${graphName}_emitted"
$buildDir = Join-Path $repoRoot "build\hostsim_${graphName}_emitted_build"

Write-Host "Stopping running HostSim / NodeGUI (unlocks emit output)..."
Stop-SimApps

$emitted = Join-Path $repoRoot "build\hostsim_${graphName}_emitted"
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

    Write-Host "Emitting ${graphName} graph into HostSim..."
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
    Write-Host "Using existing emitted ${graphName} build (pass -ForceEmit to rebuild)."
}

if (-not (Test-Path $exe)) { throw "host_sim.exe not found in $buildDir" }

Write-Host "Starting HostSim live for ${graphName}..."
Start-Process -FilePath $exe -ArgumentList $scenario, "--live", "--realtime", "1.0" -WorkingDirectory $emitted

if (-not $NoGui) {
    Start-Sleep -Seconds 1
    $guiWd = Join-Path $repoRoot "build\Source\NodeGUI"
    $graphArg = (Resolve-Path $graph).Path
    Start-Process -FilePath (Join-Path $guiWd "NodeGUI.exe") `
        -ArgumentList $graphArg, "--tcp", "127.0.0.1:14608", "--protocol", "ivp" `
        -WorkingDirectory $guiWd
    Write-Host "NodeGUI opened with ${graphName} graph + live telemetry."
}

Write-Host ""
Write-Host "Scenario: $scenario"
Write-Host "Live telemetry: 127.0.0.1:14608 (IVP)"
