#!/bin/bash
# Functional simulation of video_formatter.v with Vivado xsim.
# Usage: ./run_formatter_sim.sh [master|current]
#   master  - runs the testbench against the pre-branch 32-bit formatter
#             (validates the testbench reference model)
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

mkdir -p "$SIMDIR"
cd "$SIMDIR"

if [ "$VARIANT" = "master" ]; then
    git -C "$ROOT" show master:video_formatter.v > dut.v
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
    cmd //c "$(cygpath -w "$VIVADO_BIN/xvlog.bat") $DEFINE dut.v $EXTRA $TB" > xvlog.log 2>&1 \
        || { cat xvlog.log; exit 1; }
    cmd //c "$(cygpath -w "$VIVADO_BIN/xelab.bat") -L xpm work.video_formatter_tb -s tb" > xelab.log 2>&1 \
        || { cat xelab.log; exit 1; }
else
    "$VIVADO_BIN/xvlog" $DEFINE dut.v $EXTRA "$HERE/video_formatter_tb.v" > xvlog.log 2>&1 \
        || { cat xvlog.log; exit 1; }
    "$VIVADO_BIN/xelab" -L xpm work.video_formatter_tb -s tb > xelab.log 2>&1 \
        || { cat xelab.log; exit 1; }
fi

# CMODE SCALEX SCALEY WIDTH
CONFIGS="
2 0 0 64
1 0 0 64
0 0 0 64
3 0 0 64
2 1 0 64
1 1 0 64
0 1 0 64
1 0 0 62
2 0 1 64
1 0 1 64
2 0 1 720
2 0 0 1920
"

rm -f run_*.log
EXPECTED=$(echo "$CONFIGS" | grep -c '[0-9]')
echo "$CONFIGS" | while read -r CM SX SY W; do
    [ -z "$CM" ] && continue
    if [ "$ON_WINDOWS" = 1 ]; then
        cmd //c "$(cygpath -w "$VIVADO_BIN/xsim.bat") tb --runall --testplusarg \"CMODE=$CM\" --testplusarg \"SCALEX=$SX\" --testplusarg \"SCALEY=$SY\" --testplusarg \"WIDTH=$W\"" \
            < /dev/null > "run_${CM}_${SX}_${SY}_${W}.log" 2>&1 || true
    else
        "$VIVADO_BIN/xsim" tb --runall --testplusarg "CMODE=$CM" --testplusarg "SCALEX=$SX" --testplusarg "SCALEY=$SY" --testplusarg "WIDTH=$W" \
            < /dev/null > "run_${CM}_${SX}_${SY}_${W}.log" 2>&1 || true
    fi
    grep -E "RESULT|MISMATCH row=[0-9]+ x=(0|1|2|3|4|5|6|7) " "run_${CM}_${SX}_${SY}_${W}.log" | head -8
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
