#!/bin/bash
# Exercise the production sampler with the same pixel/raster cases as xsim.
# Test-only CDC behavior replaces XPM; Vivado still owns CDC and I/O timing.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/test/video/build/verilator_sampler"
mkdir -p "$BUILD"

verilator --binary --timing -Wno-fatal -Wno-WIDTH -Wno-PINMISSING \
    -Wno-INITIALDLY --top-module videocap_sampler_tb \
    --Mdir "$BUILD" \
    "$ROOT/videocap_sampler.v" \
    "$ROOT/videocap_calibration_capture.v" \
    "$ROOT/videocap_writeback_layout.v" \
    "$ROOT/test/video/xpm_cdc_sim.sv" \
    "$ROOT/test/video/videocap_sampler_tb.v" \
    > "$BUILD/build.log" 2>&1 || {
        cat "$BUILD/build.log"
        exit 1
    }

count=0
while read -r ps sm fw ch cv jitter shift; do
    log="$BUILD/run_${ps}_${sm}_${fw}_${ch}_${cv}_${jitter}_${shift}.log"
    if ! timeout 90s "$BUILD/Vvideocap_sampler_tb" \
            "+PIXSPAN=$ps" "+SAMPLEMODE=$sm" "+FULLWIDTH=$fw" \
            "+CROPH=$ch" "+CROPV=$cv" "+JITTER=$jitter" \
            "+GRIDSHIFT=$shift" > "$log" 2>&1; then
        cat "$log"
        exit 1
    fi
    if ! grep -q '^RESULT PASS checks=' "$log" ||
       grep -q '^RESULT FAIL\|^MISMATCH' "$log"; then
        cat "$log"
        exit 1
    fi
    grep '^RESULT PASS checks=' "$log"
    count=$((count + 1))
done < "$ROOT/test/video/videocap_cases.txt"
echo "SIM: ALL PASS ($count capture configurations)"

variant_build="$BUILD/variant_rgb"
mkdir -p "$variant_build"
verilator --binary --timing -Wno-fatal -Wno-WIDTH -Wno-PINMISSING \
    --top-module videocap_variant_tb --Mdir "$variant_build" \
    "$ROOT/videocap_sampler.v" \
    "$ROOT/videocap_calibration_capture.v" \
    "$ROOT/test/video/xpm_cdc_sim.sv" \
    "$ROOT/test/video/videocap_variant_tb.v" \
    > "$variant_build/build.log" 2>&1 || {
        cat "$variant_build/build.log"
        exit 1
    }
if ! timeout 30s "$variant_build/Vvideocap_variant_tb" \
        > "$variant_build/run.log" 2>&1; then
    cat "$variant_build/run.log"
    exit 1
fi
if ! grep -q '^RESULT PASS RGB topology:' "$variant_build/run.log" ||
   grep -q '^RESULT FAIL' "$variant_build/run.log"; then
    cat "$variant_build/run.log"
    exit 1
fi
grep '^RESULT PASS RGB topology:' "$variant_build/run.log"
