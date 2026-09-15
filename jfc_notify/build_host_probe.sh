#!/usr/bin/env bash
set -euo pipefail

probe_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
urma_include_dir=${URMA_INCLUDE_DIR:-/usr/include/ub/umdk/urma}
urma_library=${URMA_LIBRARY:-/usr/lib64/liburma.so.0.0.3}
probe_output=${PROBE_OUTPUT:-"$probe_dir/host_receive_probe"}
probe_cc=${CC:-cc}

if [[ ! -r "$urma_include_dir/urma_api.h" || ! -r "$urma_library" ]]; then
    printf 'Missing target headers/library. Set URMA_INCLUDE_DIR and URMA_LIBRARY.\n' >&2
    exit 1
fi
urma_library=$(readlink -f -- "$urma_library")
urma_library_dir=$(dirname -- "$urma_library")

"$probe_cc" -std=c11 -O2 -Wall -Wextra -pthread \
    -I"$urma_include_dir" "$probe_dir/host_receive_probe.c" \
    "$urma_library" -Wl,-rpath,"$urma_library_dir" -ldl -o "$probe_output"

printf 'Built: %s\nHeaders: %s\nLink library: %s\n' \
    "$probe_output" "$urma_include_dir" "$urma_library"
printf 'Next: run the binary with --list; it prints the URMA library actually loaded.\n'
