#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
firmware_dir="$(cd "$script_dir/../.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT
cc -std=c11 -Wall -Wextra -Werror -pedantic \
    -I"$script_dir/mock" -I"$firmware_dir/Inc" \
    "$firmware_dir/Src/Inverter/SafetyLink.c" \
    "$script_dir/test_safety_link.c" -o "$build_dir/test_safety_link"
"$build_dir/test_safety_link"
cc -std=c11 -Wall -Wextra -Werror -pedantic \
    -I"$firmware_dir/../../../Lib/SafetyFaultLink/include" \
    -c "$firmware_dir/../../../Lib/SafetyFaultLink/src/safety_fault_wire.c" \
    -o "$build_dir/safety_fault_wire.o"
c++ -std=c++17 -Wall -Wextra -Werror -pedantic \
    -I"$script_dir/mock" -I"$firmware_dir/Inc" \
    -I"$firmware_dir/../../../Lib/SafetyFaultLink/include" \
    "$firmware_dir/Src/Inverter/Control/CoprocessorFaults.cpp" \
    "$script_dir/test_coprocessor_faults.cpp" "$build_dir/safety_fault_wire.o" \
    -o "$build_dir/test_coprocessor_faults"
"$build_dir/test_coprocessor_faults"
