#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT
cc -std=c11 -Wall -Wextra -Werror -pedantic \
    "$script_dir/test_bridge_dma_math.c" \
    -o "$build_dir/test_bridge_dma_math"
"$build_dir/test_bridge_dma_math"
cc -std=c11 -Wall -Wextra -Werror -pedantic \
    "$script_dir/test_bridge_command.c" \
    -o "$build_dir/test_bridge_command"
"$build_dir/test_bridge_command"
protocol_dir="$script_dir/../../../../../../Lib/InverterProtocol"
cc -std=c11 -Wall -Wextra -Werror -pedantic \
    -I"$protocol_dir/include" \
    "$script_dir/test_bridge_startup.c" "$protocol_dir/src/protocol.c" \
    -o "$build_dir/test_bridge_startup"
"$build_dir/test_bridge_startup"
cc -std=c11 -Wall -Wextra -Werror -pedantic \
    -I"$script_dir/mock_bridge" -I"$script_dir/../../../Safety" \
    -I"$protocol_dir/include" -I"$protocol_dir/../SafetyFaultLink/include" \
    "$script_dir/test_bridge_flow.c" "$protocol_dir/src/protocol.c" \
    "$protocol_dir/../SafetyFaultLink/src/safety_fault_wire.c" \
    "$script_dir/../../../Safety/safety_policy.c" \
    -o "$build_dir/test_bridge_flow"
"$build_dir/test_bridge_flow"
