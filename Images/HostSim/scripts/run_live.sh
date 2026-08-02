#!/usr/bin/env bash
# Run the already-built HostSim base image in live mode and (optionally) open NodeGUI.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOSTSIM_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${HOSTSIM_ROOT}/../.." && pwd)"

SCENARIO="${1:-${HOSTSIM_ROOT}/scenarios/default_motor.json}"
HOSTSIM="${HOSTSIM_ROOT}/build_linux/host_sim"
NODEGUI="${REPO_ROOT}/build/Source/NodeGUI/NodeGUI"
NO_GUI=0

for arg in "$@"; do
  case "${arg}" in
    --no-gui) NO_GUI=1 ;;
  esac
done

if [[ ! -x "${HOSTSIM}" ]]; then
  echo "HostSim not found at ${HOSTSIM}" >&2
  echo "Build it first: cmake -S ${HOSTSIM_ROOT} -B ${HOSTSIM_ROOT}/build_linux && cmake --build ${HOSTSIM_ROOT}/build_linux" >&2
  exit 1
fi

pkill -f 'host_sim' 2>/dev/null || true
pkill -f 'NodeGUI' 2>/dev/null || true
sleep 0.5

echo "Starting HostSim live: ${SCENARIO}"
nohup "${HOSTSIM}" "${SCENARIO}" --live --realtime 1.0 >/dev/null 2>&1 &

if [[ "${NO_GUI}" -eq 0 ]]; then
  if [[ ! -x "${NODEGUI}" ]]; then
    echo "NodeGUI not found at ${NODEGUI}. Build with: cmake --build build --target NodeGUI" >&2
    echo "HostSim is still running on 127.0.0.1:14608"
    exit 0
  fi
  sleep 1
  nohup "${NODEGUI}" --tcp 127.0.0.1:14608 --protocol ivp >/dev/null 2>&1 &
  echo "NodeGUI opened."
fi

echo "HostSim live on 127.0.0.1:14608"
