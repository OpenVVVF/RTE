#!/usr/bin/env bash
#
# Host-side verification for the Gen7 I2C drivers.
#
# Compiles OnboardTempSensor + RailMonitor against a mock I2C master (plain
# g++ -std=c++17, no external dependencies, no HAL) and runs the assertion
# suites.  Exit code 0 = all checks passed.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
FW_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${SCRIPT_DIR}/out"

CXX="${CXX:-g++}"
CXXFLAGS=(
    -std=c++17
    -O2
    -g
    -Wall
    -Wextra
    -Wconversion
    -Wsign-conversion
    -I"${FW_ROOT}/Inc"
    -I"${SCRIPT_DIR}"
)

mkdir -p "${BUILD_DIR}"

SOURCES=(
    "${SCRIPT_DIR}/test_main.cpp"
    "${SCRIPT_DIR}/test_temp.cpp"
    "${SCRIPT_DIR}/test_rail.cpp"
    "${SCRIPT_DIR}/test_temp_adversarial.cpp"
    "${SCRIPT_DIR}/test_rail_adversarial.cpp"
    "${FW_ROOT}/Src/Inverter/Drivers/I2C/OnboardTempSensor.cpp"
    "${FW_ROOT}/Src/Inverter/Drivers/I2C/RailMonitor.cpp"
)

echo "==> Compiling host harness..."
"${CXX}" "${CXXFLAGS[@]}" "${SOURCES[@]}" -o "${BUILD_DIR}/i2c_driver_tests"

echo "==> Running tests..."
"${BUILD_DIR}/i2c_driver_tests"
