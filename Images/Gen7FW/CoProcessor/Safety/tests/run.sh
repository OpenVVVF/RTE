#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT
cc -std=c11 -Wall -Wextra -Werror -pedantic \
    -I"$script_dir/.." \
    "$script_dir/../safety_policy.c" "$script_dir/test_safety_policy.c" \
    -o "$build_dir/test_safety_policy"
"$build_dir/test_safety_policy"
cc -std=c11 -Wall -Wextra -Werror -pedantic \
    -I"$script_dir/.." \
    "$script_dir/../safety_heartbeat.c" "$script_dir/test_safety_heartbeat.c" \
    -o "$build_dir/test_safety_heartbeat"
"$build_dir/test_safety_heartbeat"
cc -std=c11 -Wall -Wextra -Werror -pedantic \
    -I"$script_dir/.." \
    "$script_dir/../safety_policy.c" \
    "$script_dir/../safety_heartbeat.c" \
    "$script_dir/test_normal_boot.c" \
    -o "$build_dir/test_normal_boot"
"$build_dir/test_normal_boot"
