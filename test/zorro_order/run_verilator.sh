#!/bin/bash
# Production mntzorro.v on the Zorro III pins:
#   axi_order_tb   read-after-write ordering through m00
#   dtack_order_tb DTACK must not lead the read data (AXI, receive window,
#                  register and ARM-answered reads)
# ZORRO_RTL names another copy of mntzorro.v, to check a test against the
# RTL it is meant to fail on.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
RTL="${ZORRO_RTL:-$ROOT/mntzorro.v}"

build() {
    local top="$1" dir="$ROOT/test/zorro_order/build/$1"
    mkdir -p "$dir"
    verilator --binary --timing -Wno-fatal -Wno-WIDTH -Wno-PINMISSING -Wno-INITIALDLY \
        --top-module "$top" --Mdir "$dir" \
        "$ROOT/test/zorro_order/rtl_primitives_sim.v" \
        "$ROOT/test/video/xpm_cdc_sim.sv" \
        "$ROOT/videocap_clock_control.v" \
        "$ROOT/videocap_calibration_capture.v" \
        "$ROOT/videocap_writeback_layout.v" \
        "$ROOT/videocap_sampler.v" \
        "$RTL" \
        "$ROOT/test/zorro_order/$top.v" \
        > "$dir/build.log" 2>&1 || {
            cat "$dir/build.log"
            exit 1
        }
}

run() {
    local top="$1"; shift
    local dir="$ROOT/test/zorro_order/build/$top" log="$ROOT/test/zorro_order/build/$top/$1.log"
    "$dir/V$top" "${@:2}" > "$log" 2>&1 || {
        cat "$log"
        exit 1
    }
}

build axi_order_tb
run axi_order_tb red +BYPASS_ORDERING
grep '^RESULT RED reproduced delayed-B stale read$' "$ROOT/test/zorro_order/build/axi_order_tb/red.log"
run axi_order_tb green
grep '^RESULT PASS m00 write/read ordering$' "$ROOT/test/zorro_order/build/axi_order_tb/green.log"

build dtack_order_tb
run dtack_order_tb green
grep '^RESULT PASS z3 read DTACK after data ' "$ROOT/test/zorro_order/build/dtack_order_tb/green.log"
