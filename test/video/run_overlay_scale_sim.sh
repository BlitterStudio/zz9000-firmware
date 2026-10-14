#!/bin/bash
# Native overlay line fetcher and scaler simulations with Vivado xsim:
# the video_overlay_linebuffer unit bench and the whole-window
# video_overlay_scale_tb scenarios below.
# Usage: ./run_overlay_scale_sim.sh
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
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SIMDIR="$HERE/build/sim_overlay_scale"
GLBL="$VIVADO_BIN/../data/verilog/src/glbl.v"

rm -rf "$SIMDIR"
mkdir -p "$SIMDIR"
cd "$SIMDIR"
cp "$ROOT/video_formatter.v" "$ROOT/video_source_sync.v" \
   "$ROOT/video_overlay_pixel.v" "$ROOT/video_overlay_linebuffer.v" .
cp "$HERE/video_overlay_scale_tb.v" "$HERE/video_overlay_linebuffer_tb.v" .
SOURCES="video_formatter.v video_source_sync.v video_overlay_pixel.v video_overlay_linebuffer.v video_overlay_scale_tb.v video_overlay_linebuffer_tb.v"

tool() {
    local name="$1"; shift
    if [ "$ON_WINDOWS" = 1 ]; then
        MSYS_NO_PATHCONV=1 cmd.exe /c "$(cygpath -w "$VIVADO_BIN/$name.bat") $*"
    else
        "$VIVADO_BIN/$name" "$@"
    fi
}

if [ "$ON_WINDOWS" = 1 ]; then
    tool xvlog $SOURCES "$(cygpath -w "$GLBL")" > /dev/null || { cat xvlog.log; exit 1; }
else
    tool xvlog $SOURCES "$GLBL" > /dev/null || { cat xvlog.log; exit 1; }
fi
tool xelab -L xpm work.video_overlay_scale_tb work.glbl -s scale > /dev/null \
    || { cat xelab.log; exit 1; }
tool xelab -L xpm work.video_overlay_linebuffer_tb work.glbl -s linebuffer > /dev/null \
    || { cat xelab.log; exit 1; }

tool xsim linebuffer --runall < /dev/null > run_linebuffer.log 2>&1 || true
grep -E "FAIL|PASS|failure" run_linebuffer.log

# One scenario per line, as plusargs over the bench defaults (160x96 screen,
# 72x58 source in a 72x41 window at 8,4: irregular 1- and 2-row steps).
SCENARIOS="
default
OVL_GAP_EVERY=2 OVL_GAP_CYCLES=3
POS_X=30 POS_Y=60
POS_X=-10 POS_Y=-7
SRC_W=36 SRC_H=20
SRC_W=72 SRC_H=41
SRC_W=72 SRC_H=80
SRC_W=100 SRC_H=41
"
# The stream stall above delivers one source row in about 60% of a raster
# line: two rows never fit in one line, so only a fetcher that runs ahead of
# the display keeps every row on time.

n=0
echo "$SCENARIOS" | while read -r ARGS; do
    [ -z "$ARGS" ] && continue
    n=$((n + 1))
    PLUS=""
    if [ "$ARGS" != default ]; then
        for a in $ARGS; do
            if [ "$ON_WINDOWS" = 1 ]; then
                PLUS="$PLUS --testplusarg \"$a\""
            else
                PLUS="$PLUS --testplusarg $a"
            fi
        done
    fi
    if [ "$ON_WINDOWS" = 1 ]; then
        MSYS_NO_PATHCONV=1 cmd.exe /c "$(cygpath -w "$VIVADO_BIN/xsim.bat") scale --runall $PLUS" \
            < /dev/null > "run_scale_$n.log" 2>&1 || true
    else
        # shellcheck disable=SC2086
        "$VIVADO_BIN/xsim" scale --runall $PLUS < /dev/null > "run_scale_$n.log" 2>&1 || true
    fi
    echo "[$ARGS] $(grep RESULT "run_scale_$n.log" || echo 'no RESULT')"
done

EXPECTED=$(echo "$SCENARIOS" | grep -c '[a-z0-9]')
RESULTS=$(cat run_scale_*.log | grep -c "RESULT" || true)
if ! grep -q "video_overlay_linebuffer_tb: PASS" run_linebuffer.log; then
    echo "SIM: LINEBUFFER FAILED"
    exit 1
fi
if [ "$RESULTS" -ne "$EXPECTED" ]; then
    echo "SIM: INCOMPLETE ($RESULTS/$EXPECTED scenarios reported)"
    exit 1
fi
if cat run_scale_*.log | grep "RESULT" | grep -qv "MISMATCHES=0"; then
    echo "SIM: FAILURES PRESENT"
    exit 1
fi
echo "SIM: ALL PASS (linebuffer + $RESULTS scenarios)"
