#!/usr/bin/env bash
set -euo pipefail

probe_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
probe_sdk=${CANN_ROOT:-${ASCEND_AICPU_PATH:-/usr/local/Ascend/ascend-toolkit/latest}}
probe_cxx=${CXX:-c++}
probe_output=${PROBE_OUTPUT:-"$probe_dir/aicpu_urma_probe"}
runtime_library=${RUNTIME_LIBRARY:-}
runtime_include_dir=${RUNTIME_INCLUDE_DIR:-}
protocol_dir=${PROBE_PROTOCOL_DIR:-}
urma_include_dir=${URMA_INCLUDE_DIR:-/usr/include/ub/umdk/urma}
include_args=()
urma_args=()

# Installed packages place public Runtime and dependent profiling headers in different roots.
for prefix in "$probe_sdk/aarch64-linux" "$probe_sdk"; do
    for suffix in include pkg_inc pkg_inc/runtime include/experiment/runtime; do
        candidate="$prefix/$suffix"
        if [[ -d "$candidate" ]]; then
            include_args+=(-I"$candidate")
            if [[ -z "$runtime_include_dir" && -r "$candidate/runtime/kernel.h" ]]; then
                runtime_include_dir=$candidate
            fi
        fi
    done
    if [[ -z "$runtime_library" && -r "$prefix/lib64/libruntime.so" ]]; then
        runtime_library="$prefix/lib64/libruntime.so"
    fi
done
if [[ -z "$protocol_dir" ]]; then
    for candidate in "$probe_dir" \
        "${AICPU_ROOT:-$probe_dir/../../AscendCCv2-AICPU}/ms_kernels/src/secure_dma"; do
        if [[ -r "$candidate/secure_dma_urma_probe_protocol.h" ]]; then
            protocol_dir=$candidate
            break
        fi
    done
fi
if [[ ! -r "$runtime_include_dir/runtime/kernel.h" || ! -r "$runtime_library" ]]; then
    printf 'Missing Runtime headers/library. Set CANN_ROOT, or RUNTIME_INCLUDE_DIR and RUNTIME_LIBRARY.\n' >&2
    exit 1
fi
if [[ ! -r "$protocol_dir/secure_dma_urma_probe_protocol.h" || ! -r "$protocol_dir/secure_dma_urma_probe_abi.h" ]]; then
    printf 'Copy secure_dma_urma_probe_protocol.h and secure_dma_urma_probe_abi.h beside this script, or set AICPU_ROOT / PROBE_PROTOCOL_DIR.\n' >&2
    exit 1
fi
if [[ -r "$urma_include_dir/urma_api.h" ]]; then
    urma_args+=(-I"$urma_include_dir" -DSECURE_DMA_PROBE_HAVE_URMA_HEADERS=1)
else
    printf 'URMA headers absent: building symbol mode only. Set URMA_INCLUDE_DIR to enable --list / --resources.\n' >&2
fi
runtime_library=$(readlink -f -- "$runtime_library")
runtime_library_dir=$(dirname -- "$runtime_library")
"$probe_cxx" -std=c++14 -O2 -Wall -Wextra -Werror \
    -I"$runtime_include_dir" -I"$runtime_include_dir/.." "${include_args[@]}" \
    -I"$protocol_dir" "${urma_args[@]}" "$probe_dir/aicpu_urma_probe.cpp" \
    "$runtime_library" -Wl,-rpath,"$runtime_library_dir" -ldl -o "$probe_output"

printf 'Built: %s\nHeaders: %s\nLink library: %s\nProtocol: %s\n' \
    "$probe_output" "$runtime_include_dir" "$runtime_library" "$protocol_dir/secure_dma_urma_probe_protocol.h"
printf 'URMA headers: %s\n' "$urma_include_dir"
if [[ ${#urma_args[@]} -gt 0 ]]; then
    printf 'After deploying the version-2 CI AICPU package: run %s --device 0 --list\n' "$probe_output"
else
    printf 'Symbol mode only: run %s --device 0\n' "$probe_output"
fi
