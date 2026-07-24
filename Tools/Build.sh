#!/usr/bin/env bash
set -euo pipefail

# Build all executable targets in the RTE project.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"

echo "Configuring RTE project ..."
cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}"

echo "Building all executable targets ..."
cmake --build "${BUILD_DIR}" --parallel

echo ""
echo "Build complete. Executables are in ${BUILD_DIR}."
