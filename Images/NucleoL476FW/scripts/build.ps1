# Build the NucleoL476FW base image on Windows using the STM32CubeIDE-bundled
# ARM GCC toolchain.  Usage:  powershell -File scripts\build.ps1 [-Source <dir>] [-BuildDir <dir>]
param(
    [string]$Source = (Split-Path $PSScriptRoot -Parent),
    [string]$BuildDir = "",
    [string]$BuildType = "Debug"
)

$ErrorActionPreference = "Stop"

if ($BuildDir -eq "") {
    # Keep the build tree OUTSIDE the firmware source tree: RTECodeEmitter
    # copies the whole base tree, so an in-tree build dir would get copied too.
    $repoRoot = Split-Path (Split-Path $Source -Parent) -Parent
    $BuildDir = Join-Path $repoRoot "build\nucleo_fw_build"
}

# Locate the ARM GCC bundled with STM32CubeIDE (any version).
$gccPlugin = Get-ChildItem "C:\ST\STM32CubeIDE*\STM32CubeIDE\plugins\*" -Directory -ErrorAction SilentlyContinue |
    Where-Object Name -like "com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32*" |
    Select-Object -First 1
if (-not $gccPlugin) { throw "STM32CubeIDE ARM GCC plugin not found under C:\ST" }
$gccBin = Join-Path $gccPlugin.FullName "tools\bin"

$env:PATH = "$gccBin;" + $env:PATH

# Ninja: use one on PATH, else a cached local copy, else download it once.
$ninja = (Get-Command ninja -ErrorAction SilentlyContinue).Source
if (-not $ninja) {
    $ninjaDir = Join-Path $env:LOCALAPPDATA "rte-tools"
    $ninja = Join-Path $ninjaDir "ninja.exe"
    if (-not (Test-Path $ninja)) {
        New-Item -ItemType Directory -Force -Path $ninjaDir | Out-Null
        $zip = Join-Path $ninjaDir "ninja-win.zip"
        Invoke-WebRequest -Uri "https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip" -OutFile $zip
        Expand-Archive $zip -DestinationPath $ninjaDir -Force
        Remove-Item $zip
    }
}
$genArgs = @("-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=$ninja")

cmake -S $Source -B $BuildDir @genArgs `
    "-DCMAKE_TOOLCHAIN_FILE=$Source\cmake\gcc-arm-none-eabi.cmake" `
    "-DCMAKE_BUILD_TYPE=$BuildType"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

cmake --build $BuildDir -j
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

Write-Host "`nELF: $BuildDir\STM32CubeMX.elf"
Write-Host "BIN: $BuildDir\STM32CubeMX.bin"
