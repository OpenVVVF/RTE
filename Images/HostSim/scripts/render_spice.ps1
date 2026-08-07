# Run an accurate ngspice batch render for a user-specified time window.
# The resulting trace CSV has enough temporal resolution to resolve ~20th
# harmonic of the motor electrical frequency when tim_isr_hz is chosen as
#   tim_isr_hz >= 20 * f_elec_hz * points_per_cycle
# (points_per_cycle ~ 20 for a clean waveform).
#
# Usage:
#   .\render_spice.ps1 -Duration 0.005 -TimIsrHz 50000 -Scenario scenarios\accurate_spice.json
param(
    [float]$Duration = 0.005,
    [float]$TimIsrHz = 50000,
    [int]$Substeps = 1,
    [string]$Scenario = "scenarios\accurate_spice.json",
    [string]$Trace = "accurate_spice_trace.csv"
)

$ErrorActionPreference = "Stop"

$hostSimRoot = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $hostSimRoot "build\Debug\host_sim.exe"

if (-not (Test-Path $exe)) {
    $exe = Join-Path $hostSimRoot "build\host_sim.exe"
}

if (-not (Test-Path $exe)) {
    Write-Host "Building HostSim..."
    cmake -S $hostSimRoot -B (Join-Path $hostSimRoot "build")
    cmake --build (Join-Path $hostSimRoot "build") --config Debug
}

Write-Host "Rendering accurate SPICE window: $Duration s at $TimIsrHz Hz ISR, $Substeps substeps"
Write-Host "Trace: $Trace"

Push-Location $hostSimRoot
try {
    & $exe $Scenario `
        --plant-backend ngspice `
        --duration $Duration `
        --tim-isr-hz $TimIsrHz `
        --substeps $Substeps
    Write-Host "Done. Plot with: python scripts\plot_sim.py $Trace"
} finally {
    Pop-Location
}
