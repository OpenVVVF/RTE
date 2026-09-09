# Run HostSim with ngspice plant simulation live + launch NodeGUI visualization.
param(
    [switch]$NoGui
)

$ErrorActionPreference = "Stop"

$hostSimRoot = Split-Path $PSScriptRoot -Parent
$repoRoot = Split-Path (Split-Path $hostSimRoot -Parent) -Parent
$scenario = Join-Path $hostSimRoot "scenarios\ngspice_rl_demo.json"
$exe = Join-Path $hostSimRoot "build\Debug\host_sim.exe"

if (-not (Test-Path $exe)) {
    $exe = Join-Path $hostSimRoot "build\host_sim.exe"
}

if (-not (Test-Path $exe)) {
    Write-Host "Building HostSim..."
    cmake -S $hostSimRoot -B (Join-Path $hostSimRoot "build")
    cmake --build (Join-Path $hostSimRoot "build") --config Debug
}

function Stop-SimApps {
    Get-Process | Where-Object { $_.ProcessName -match '^(host_sim|NodeGUI)$' } |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
}

Write-Host "Stopping running HostSim / NodeGUI..."
Stop-SimApps

Write-Host "Starting HostSim with ngspice circuit plant..."
Start-Process -FilePath $exe -ArgumentList "--scenario", $scenario, "--live", "--realtime", "1.0" -WorkingDirectory $hostSimRoot

if (-not $NoGui) {
    Start-Sleep -Seconds 1
    $guiExe = Join-Path $repoRoot "build\Source\NodeGUI\NodeGUI.exe"
    if (Test-Path $guiExe) {
        $guiWd = Split-Path $guiExe
        Start-Process -FilePath $guiExe `
            -ArgumentList "--tcp", "127.0.0.1:14608", "--protocol", "ivp" `
            -WorkingDirectory $guiWd
        Write-Host "NodeGUI opened with ngspice live telemetry."
    } else {
        Write-Host "NodeGUI executable not found at $guiExe. Build NodeGUI using: cmake --build build --target NodeGUI"
    }
}
