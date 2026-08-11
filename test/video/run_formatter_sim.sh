#!/bin/bash
# Functional simulation of video_formatter.v with Vivado xsim.
# Usage: ./run_formatter_sim.sh [reference|master|current]
#   reference/master - runs against the pinned pre-64-bit formatter
#                      (validates the testbench reference model)
#   current - runs against the working-tree video_formatter.v (default)
set -e

# Windows Git Bash drives the .bat tools through cmd (quoting the plusargs,
# which batch files would otherwise split on '='); Linux runs them directly.
if command -v cygpath >/dev/null 2>&1; then
    ON_WINDOWS=1
    VIVADO_BIN="${VIVADO_BIN:-D:/Xilinx/Vivado/2018.3/bin}"
else
    ON_WINDOWS=0
    VIVADO_BIN="${VIVADO_BIN:-/opt/Xilinx/Vivado/2018.3/bin}"
fi
VARIANT="${1:-current}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SIMDIR="$HERE/build/sim_$VARIANT"
GLBL="$VIVADO_BIN/../data/verilog/src/glbl.v"
REFERENCE_COMMIT="${FORMATTER_REFERENCE_COMMIT:-6e0cfc53b0135a271dc71d31b29dc864fc36dddb}"

mkdir -p "$SIMDIR"
cd "$SIMDIR"

if [ "$VARIANT" = "master" ] || [ "$VARIANT" = "reference" ]; then
    git -C "$ROOT" show "$REFERENCE_COMMIT:video_formatter.v" > dut.v
    DEFINE="-d MASTER_DUT"
    EXTRA=""
else
    cp "$ROOT/video_formatter.v" dut.v
    cp "$ROOT/video_overlay_pixel.v" .
    cp "$ROOT/video_overlay_linebuffer.v" .
    DEFINE=""
    EXTRA="video_overlay_pixel.v video_overlay_linebuffer.v"
fi

if [ "$ON_WINDOWS" = 1 ]; then
    TB="$(cygpath -w "$HERE/video_formatter_tb.v")"
    cmd //c "$(cygpath -w "$VIVADO_BIN/xvlog.bat") $DEFINE dut.v $EXTRA $TB $(cygpath -w "$GLBL")" > xvlog.log 2>&1 \
        || { cat xvlog.log; exit 1; }
    cmd //c "$(cygpath -w "$VIVADO_BIN/xelab.bat") -L xpm work.video_formatter_tb work.glbl -s tb" > xelab.log 2>&1 \
        || { cat xelab.log; exit 1; }
else
    "$VIVADO_BIN/xvlog" $DEFINE dut.v $EXTRA "$HERE/video_formatter_tb.v" "$GLBL" > xvlog.log 2>&1 \
        || { cat xvlog.log; exit 1; }
    "$VIVADO_BIN/xelab" -L xpm work.video_formatter_tb work.glbl -s tb > xelab.log 2>&1 \
        || { cat xelab.log; exit 1; }
fi

# CMODE SCALEX SCALEY WIDTH STREAM_GAP_EVERY STREAM_GAP_CYCLES INTERLACE
CONFIGS="
2 0 0 64 0 0 0
1 0 0 64 0 0 0
0 0 0 64 0 0 0
3 0 0 64 0 0 0
2 1 0 64 0 0 0
1 1 0 64 0 0 0
0 1 0 64 0 0 0
1 0 0 62 0 0 0
2 0 1 64 0 0 0
1 0 1 64 0 0 0
"

# The wide-line cases exercise capacity added with the 64-bit formatter.
# They are not valid expectations for the pinned 32-bit reference design.
if [ "$VARIANT" != "master" ] && [ "$VARIANT" != "reference" ]; then
    CONFIGS="$CONFIGS
2 0 1 720 0 0 0
2 0 2 720 0 0 0
1 0 2 62 0 0 0
2 0 0 1920 0 0 0
2 0 2 1280 16 32 0
2 0 0 64 0 0 1
2 0 1 64 0 0 1
"
fi

rm -f run_*.log
EXPECTED=$(echo "$CONFIGS" | grep -c '[0-9]')
echo "$CONFIGS" | while read -r CM SX SY W GE GC IL; do
    [ -z "$CM" ] && continue
    if [ "$ON_WINDOWS" = 1 ]; then
        cmd //c "$(cygpath -w "$VIVADO_BIN/xsim.bat") tb --runall --testplusarg \"CMODE=$CM\" --testplusarg \"SCALEX=$SX\" --testplusarg \"SCALEY=$SY\" --testplusarg \"WIDTH=$W\" --testplusarg \"STREAM_GAP_EVERY=$GE\" --testplusarg \"STREAM_GAP_CYCLES=$GC\" --testplusarg \"INTERLACE=$IL\"" \
            < /dev/null > "run_${CM}_${SX}_${SY}_${W}_${GE}_${GC}_${IL}.log" 2>&1 || true
    else
        "$VIVADO_BIN/xsim" tb --runall --testplusarg "CMODE=$CM" --testplusarg "SCALEX=$SX" --testplusarg "SCALEY=$SY" --testplusarg "WIDTH=$W" --testplusarg "STREAM_GAP_EVERY=$GE" --testplusarg "STREAM_GAP_CYCLES=$GC" --testplusarg "INTERLACE=$IL" \
            < /dev/null > "run_${CM}_${SX}_${SY}_${W}_${GE}_${GC}_${IL}.log" 2>&1 || true
    fi
    grep -E "RESULT|MISMATCH row=[0-9]+ x=(0|1|2|3|4|5|6|7) " "run_${CM}_${SX}_${SY}_${W}_${GE}_${GC}_${IL}.log" | head -8
done

echo "---- summary ($VARIANT) ----"
grep -h "RESULT" run_*.log
RESULTS=$(grep -h "RESULT" run_*.log | wc -l)
if [ "$RESULTS" -ne "$EXPECTED" ]; then
    # a run that crashes before the testbench prints RESULT must not be
    # silently dropped from the verdict
    if grep -h -q "PrivateChannel: Error connecting to server socket" run_*.log; then
        echo "SIM: xsim localhost IPC failed; on Windows, stop Docker Desktop and retry"
    fi
    echo "SIM: MISSING RESULTS ($RESULTS of $EXPECTED configs reported)"
    exit 1
fi
if grep -h "RESULT" run_*.log | grep -qv "MISMATCHES=0"; then
    echo "SIM: FAILURES PRESENT"
    exit 1
fi
echo "SIM: ALL PASS ($RESULTS/$EXPECTED configs)"
