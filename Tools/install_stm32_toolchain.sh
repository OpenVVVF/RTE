#!/usr/bin/env bash
set -euo pipefail

# Download the xPack GNU Arm Embedded GCC toolchain into a project-local .tools/
# directory so RTEFirmwareBuilder can find it without relying on distro packages.

VERSION="14.2.1-1.1"
ARCHIVE="xpack-arm-none-eabi-gcc-${VERSION}-linux-x64.tar.gz"
URL="https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/download/v${VERSION}/${ARCHIVE}"
SHA256="ed8c7d207a85d00da22b90cf80ab3b0b2c7600509afadf6b7149644e9d4790a6"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TOOLS_DIR="${PROJECT_ROOT}/.tools"
INSTALL_DIR="${TOOLS_DIR}/xpack-arm-none-eabi-gcc-${VERSION}"

mkdir -p "${TOOLS_DIR}"
cd "${TOOLS_DIR}"

if [ -d "${INSTALL_DIR}" ]; then
    echo "Toolchain already installed at ${INSTALL_DIR}"
    echo "Add to PATH: export PATH=\"${INSTALL_DIR}/bin:\$PATH\""
    exit 0
fi

TMP_ARCHIVE="$(mktemp -p "${TOOLS_DIR}" "${ARCHIVE}.XXXXXX")"
trap 'rm -f "${TMP_ARCHIVE}"' EXIT

echo "Downloading ${URL} ..."
curl -fsSL -o "${TMP_ARCHIVE}" "${URL}"

echo "Verifying SHA-256 checksum ..."
printf '%s  %s\n' "${SHA256}" "${TMP_ARCHIVE}" | sha256sum -c -

echo "Extracting archive ..."
tar -xzf "${TMP_ARCHIVE}"
rm -f "${TMP_ARCHIVE}"

echo ""
echo "Toolchain installed at: ${INSTALL_DIR}"
echo ""
echo "To use it manually, add the bin directory to your PATH:"
echo "  export PATH=\"${INSTALL_DIR}/bin:\$PATH\""
echo ""
echo "RTEFirmwareBuilder will also auto-detect this local toolchain when run from ${PROJECT_ROOT}."
