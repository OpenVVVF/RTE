# Flash an ELF to the Nucleo-L476RG over the on-board ST-Link using the
# OpenOCD bundled with STM32CubeIDE (works with old ST-Link V2-1 firmware
# that STM32CubeProgrammer rejects).
# Usage:  powershell -File scripts\flash.ps1 [-Elf <path>]
param(
    [string]$Elf = ""
)

$ErrorActionPreference = "Stop"

if ($Elf -eq "") {
    $fwRoot = Split-Path $PSScriptRoot -Parent
    $repoRoot = Split-Path (Split-Path $fwRoot -Parent) -Parent
    $Elf = Join-Path $repoRoot "build\nucleo_fw_build\STM32CubeMX.elf"
}
if (-not (Test-Path $Elf)) { throw "ELF not found: $Elf (build first: scripts\build.ps1)" }

$plugins = Get-ChildItem "C:\ST\STM32CubeIDE*\STM32CubeIDE\plugins\*" -Directory -ErrorAction SilentlyContinue
$oocdPlugin = $plugins | Where-Object Name -like "com.st.stm32cube.ide.mcu.externaltools.openocd*" | Select-Object -First 1
$scriptsPlugin = $plugins | Where-Object Name -like "com.st.stm32cube.ide.mcu.debug.openocd*" | Select-Object -First 1
if (-not $oocdPlugin -or -not $scriptsPlugin) { throw "STM32CubeIDE OpenOCD plugins not found under C:\ST" }

$openocd = Join-Path $oocdPlugin.FullName "tools\bin\openocd.exe"
$oocdScripts = Join-Path $scriptsPlugin.FullName "resources\openocd\st_scripts"

# Forward slashes for the TCL 'program' command.
$elfTcl = ($Elf -replace '\\', '/')

& $openocd -s $oocdScripts `
    -f interface/stlink-dap.cfg `
    -f target/stm32l4x.cfg `
    -c "program $elfTcl verify reset exit"
if ($LASTEXITCODE -ne 0) { throw "OpenOCD flash failed" }

Write-Host "`nFlashed and reset: $Elf"
