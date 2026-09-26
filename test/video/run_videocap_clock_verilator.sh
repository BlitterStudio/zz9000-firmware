#!/bin/bash
# Exercise production C28 and E7M clock-control recovery and phase requests.
# Vendor MMCM phase displacement and routed I/O timing still require Vivado.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/test/video/build/verilator_clock"

for mode in c28 legacy; do
    if [ "$mode" = c28 ]; then
        c28=1
    else
        c28=0
    fi
    case_dir="$BUILD/$mode"
    mkdir -p "$case_dir"
    printf '`define MODEL_C28 %d\n' "$c28" > "$case_dir/clock_case.vh"

    verilator --binary --timing --language 1364-2005 -Wno-WIDTH \
        -I"$case_dir" --top-module videocap_clock_control_tb \
        --Mdir "$case_dir" \
        "$ROOT/videocap_clock_control.v" \
        "$ROOT/test/video/videocap_clock_control_tb.v" \
        > "$case_dir/build.log" 2>&1 || {
            cat "$case_dir/build.log"
            exit 1
        }

    "$case_dir/Vvideocap_clock_control_tb" > "$case_dir/run.log" 2>&1 || {
        cat "$case_dir/run.log"
        exit 1
    }
    if ! grep -q '^RESULT PASS clock control:' "$case_dir/run.log" ||
       grep -q '^RESULT FAIL' "$case_dir/run.log"; then
        cat "$case_dir/run.log"
        exit 1
    fi
    grep '^RESULT PASS clock control:' "$case_dir/run.log"
done
