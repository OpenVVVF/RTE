#!/usr/bin/env bash
set -euo pipefail

# Build script for MainProcessor firmware image.
# Usage: ./build.sh [Debug|Release]
# Produces .elf, .bin, and .hex in build/<Debug|Release>/

cd "$(dirname "$0")"

BUILD_TYPE="${1:-Debug}"
case "$BUILD_TYPE" in
  Debug|Release) ;;
  *) echo "Usage: $0 [Debug|Release]"; exit 1 ;;
esac

echo "==> Configuring $BUILD_TYPE build..."
cmake --preset "$BUILD_TYPE"

echo "==> Building $BUILD_TYPE..."
cmake --build --preset "$BUILD_TYPE"

ELF="build/${BUILD_TYPE}/MainProcessor.elf"
BIN="build/${BUILD_TYPE}/MainProcessor.bin"
HEX="build/${BUILD_TYPE}/MainProcessor.hex"

echo "==> Generating firmware images..."
arm-none-eabi-objcopy -O binary "$ELF" "$BIN"
arm-none-eabi-objcopy -O ihex "$ELF" "$HEX"

echo "==> Image sizes:"
arm-none-eabi-size "$ELF"

echo "==> Build complete:"
echo "    ELF: $ELF"
echo "    BIN: $BIN"
echo "    HEX: $HEX"
