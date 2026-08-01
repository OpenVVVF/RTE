param(
    [switch]$NoGui,
    [switch]$ForceEmit,
    [switch]$KeepGui
)

$ErrorActionPreference = "Stop"

$hostSimRoot = Split-Path $PSScriptRoot -Parent
$repoRoot = Split-Path (Split-Path $hostSimRoot -Parent) -Parent
$graph = Join-Path $repoRoot "Images\NucleoL476FW\svpwm_demo_graph.json"
$scenario = Join-Path $hostSimRoot "scenarios\svpwm_live.json"

$emitter = Join-Path $repoRoot "build\Source\RTECodeEmitter\RTECodeEmitter.exe"

function Stop-SimApps {
    Get-Process | Where-Object { $_.ProcessName -match 'host_sim|Tracker|cl' } |
        Stop-Process -Force -ErrorAction SilentlyContinue
    if (-not $KeepGui) {
        Get-Process | Where-Object { $_.ProcessName -eq 'NodeGUI' } |
            Stop-Process -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Milliseconds 500
}

function Clear-EmittedTree([string]$repoRoot, [string]$relativePath) {
    $winPath = Join-Path $repoRoot $relativePath
    if (Test-Path $winPath) {
        Get-ChildItem -Path $winPath -Recurse -Force -ErrorAction SilentlyContinue | ForEach-Object {
            Set-ItemProperty -Path $_.FullName -Name IsReadOnly -Value $false -ErrorAction SilentlyContinue
        }
        try {
            Remove-Item -LiteralPath $winPath -Recurse -Force -ErrorAction Stop
        } catch {
            $trashPath = $winPath + "_old_" + (Get-Random)
            Rename-Item -Path $winPath -NewName (Split-Path $trashPath -Leaf) -ErrorAction SilentlyContinue
            Remove-Item -LiteralPath $trashPath -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

$emittedRel = "build/hostsim_svpwm_emitted"
$buildDir = Join-Path $repoRoot "build\hostsim_svpwm_emitted_build"

Write-Host "Stopping running HostSim / NodeGUI..."
Stop-SimApps

$emitted = Join-Path $repoRoot "build\hostsim_svpwm_emitted"
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

    Write-Host "Emitting SVPWM graph for HostSim..."
    $baseSrc = Join-Path $repoRoot "Images\HostSim"
    & $emitter --base-src $baseSrc --graph $graph --output $emitted --verbosity info
    if ($LASTEXITCODE -ne 0) { throw "RTECodeEmitter failed" }

    cmake -S $emitted -B $buildDir
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
    cmake --build $buildDir --config Debug
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

    $exe = Join-Path $buildDir "Debug\host_sim.exe"
    if (-not (Test-Path $exe)) { $exe = Join-Path $buildDir "host_sim.exe" }
} else {
    Write-Host "Using existing emitted SVPWM build (pass -ForceEmit to rebuild)."
}

if (-not (Test-Path $exe)) { throw "host_sim.exe not found in $buildDir" }

Write-Host "Starting HostSim SVPWM live server..."
Start-Process -FilePath $exe -ArgumentList $scenario, "--live", "--realtime", "1.0", "--telem-hz", "2000" -WorkingDirectory $emitted

if (-not $NoGui -and -not $KeepGui) {
    Start-Sleep -Seconds 1
    $guiWd = Join-Path $repoRoot "build\Source\NodeGUI"
    $graphArg = (Resolve-Path $graph).Path
    Start-Process -FilePath (Join-Path $guiWd "NodeGUI.exe") `
        -ArgumentList $graphArg, "--tcp", "127.0.0.1:14608", "--protocol", "ivp" `
        -WorkingDirectory $guiWd
    Write-Host "NodeGUI opened with SVPWM graph + live telemetry."
}

Write-Host ""
Write-Host "Live controls in NodeGUI console:"
Write-Host "  throttle a 0.7  -> modulation index (0..1.15)"
Write-Host "  throttle b 0.4  -> electrical frequency map (1..50 Hz)"
