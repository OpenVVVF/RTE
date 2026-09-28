#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT
cc -std=c11 -Wall -Wextra -Werror -pedantic \
    "$script_dir/test_bridge_dma_math.c" \
    -o "$build_dir/test_bridge_dma_math"
"$build_dir/test_bridge_dma_math"
