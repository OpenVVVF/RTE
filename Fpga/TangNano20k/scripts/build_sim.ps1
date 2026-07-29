# Build and run Icarus Verilog sims (fail-fast).
# Usage (PowerShell):  .\scripts\build_sim.ps1
# Usage (bash):        ./scripts/build_sim.sh

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $Root

function Find-Iverilog {
    $cmd = Get-Command iverilog -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $candidates = @(
        "C:\iverilog\bin\iverilog.exe",
        "C:\Program Files\Icarus Verilog\bin\iverilog.exe",
        "C:\Program Files (x86)\Icarus Verilog\bin\iverilog.exe",
        "$env:USERPROFILE\iverilog\bin\iverilog.exe"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return $c }
    }
    return $null
}

$Iverilog = Find-Iverilog
if (-not $Iverilog) {
    Write-Error "iverilog not found. Install Icarus Verilog and re-run."
}
$BinDir = Split-Path -Parent $Iverilog
$env:Path = "$BinDir;$env:Path"
$Vvp = Join-Path $BinDir "vvp.exe"

New-Item -ItemType Directory -Force -Path "build\sim" | Out-Null

Write-Host "==> tb_pwm"
& $Iverilog -g2005-sv -o build\sim\tb_pwm.vvp `
    rtl\deadtime_pair.v `
    rtl\pwm_complementary.v `
    tb\tb_pwm.v
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Push-Location build\sim
& $Vvp tb_pwm.vvp
$code = $LASTEXITCODE
Pop-Location
if ($code -ne 0) { exit $code }

Write-Host "==> tb_spi"
& $Iverilog -g2005-sv -o build\sim\tb_spi.vvp `
    rtl\spi_regs.v `
    tb\tb_spi.v
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Push-Location build\sim
& $Vvp tb_spi.vvp
$code = $LASTEXITCODE
Pop-Location
if ($code -ne 0) { exit $code }

Write-Host "ALL SIMS PASSED"
exit 0
