#!/usr/bin/env bash
set -euo pipefail
module_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_bin="$(mktemp)"
trap 'rm -f "$test_bin"' EXIT
cc -std=c11 -Wall -Wextra -Werror -pedantic \
    -I"$module_dir/include" "$module_dir/src/safety_fault_wire.c" \
    "$module_dir/tests/test_safety_fault_wire.c" -o "$test_bin"
"$test_bin"
