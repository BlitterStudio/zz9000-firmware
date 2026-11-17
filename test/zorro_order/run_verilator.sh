#!/bin/bash
# Read-after-write ordering through production mntzorro.v.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/test/zorro_order/build/verilator"
mkdir -p "$BUILD"

verilator --binary --timing -Wno-fatal -Wno-WIDTH -Wno-PINMISSING -Wno-INITIALDLY \
    --top-module axi_order_tb --Mdir "$BUILD" \
    "$ROOT/test/zorro_order/rtl_primitives_sim.v" \
    "$ROOT/test/video/xpm_cdc_sim.sv" \
    "$ROOT/videocap_clock_control.v" \
    "$ROOT/videocap_calibration_capture.v" \
    "$ROOT/videocap_writeback_layout.v" \
    "$ROOT/videocap_sampler.v" \
    "$ROOT/mntzorro.v" \
    "$ROOT/test/zorro_order/axi_order_tb.v" \
    > "$BUILD/build.log" 2>&1 || {
        cat "$BUILD/build.log"
        exit 1
    }

"$BUILD/Vaxi_order_tb" +BYPASS_ORDERING > "$BUILD/red.log" 2>&1 || {
    cat "$BUILD/red.log"
    exit 1
}
grep '^RESULT RED reproduced delayed-B stale read$' "$BUILD/red.log"

"$BUILD/Vaxi_order_tb" > "$BUILD/green.log" 2>&1 || {
    cat "$BUILD/green.log"
    exit 1
}
grep '^RESULT PASS m00 write/read ordering$' "$BUILD/green.log"
