#!/bin/bash
# Ensure white-primary GL_MODULATE preserves RGBA texels in the RTL TexEnv.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/test/rasterix/build/texenv_modulate"
mkdir -p "$BUILD"

verilator --binary --timing -Wno-fatal -Wno-WIDTH -Wno-PINMISSING \
    -I"$ROOT/rtl/rasterix/RasterIX" \
    -I"$ROOT/rtl/rasterix/Float/rtl/float" \
    --top-module texenv_modulate_tb --Mdir "$BUILD" \
    "$ROOT/rtl/rasterix/RasterIX/ColorMixerSigned.v" \
    "$ROOT/rtl/rasterix/RasterIX/TexEnv.v" \
    "$ROOT/rtl/rasterix/Float/rtl/float/ValueDelay.v" \
    "$ROOT/test/rasterix/texenv_modulate_tb.v" > "$BUILD/build.log" 2>&1 || {
        cat "$BUILD/build.log"
        exit 1
    }

if ! "$BUILD/Vtexenv_modulate_tb" > "$BUILD/run.log" 2>&1; then
    cat "$BUILD/run.log"
    exit 1
fi
if ! grep -q '^RESULT PASS checks=3$' "$BUILD/run.log" ||
   grep -q '^MISMATCH\|^RESULT FAIL' "$BUILD/run.log"; then
    cat "$BUILD/run.log"
    exit 1
fi
grep '^RESULT PASS checks=3$' "$BUILD/run.log"
