#!/bin/bash
# Source-sync simulations for video_source_sync.v with Vivado xsim.
# Usage: ./run_source_sync_sim.sh [smoke]
#   (default) - full controller cadence sweep + formatter integration
#               (progressive and woven interlace)
#   smoke     - shortened segments for quick iteration
set -e

# Windows Git Bash drives the .bat tools through cmd (quoting the
# plusargs, which batch files would otherwise split on '='); Linux runs
# them directly.
if command -v cygpath >/dev/null 2>&1; then
    ON_WINDOWS=1
    VIVADO_BIN="${VIVADO_BIN:-D:/Xilinx/Vivado/2018.3/bin}"
else
    ON_WINDOWS=0
    VIVADO_BIN="${VIVADO_BIN:-/opt/Xilinx/Vivado/2018.3/bin}"
fi

SMOKE="${1:-}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SIMDIR="$HERE/build/sim_source_sync"
GLBL="$VIVADO_BIN/../data/verilog/src/glbl.v"

mkdir -p "$SIMDIR"
cd "$SIMDIR"

cp "$ROOT/video_formatter.v" dut.v
cp "$ROOT/video_source_sync.v" .
cp "$ROOT/video_overlay_pixel.v" .
cp "$ROOT/video_overlay_linebuffer.v" .

if [ "$ON_WINDOWS" = 1 ]; then
    TBS="$(cygpath -w "$HERE/video_source_sync_tb.v") $(cygpath -w "$HERE/video_source_sync_formatter_tb.v")"
    cmd //c "$(cygpath -w "$VIVADO_BIN/xvlog.bat") dut.v video_source_sync.v video_overlay_pixel.v video_overlay_linebuffer.v $TBS $(cygpath -w "$GLBL")" > xvlog.log 2>&1 \
        || { cat xvlog.log; exit 1; }
    cmd //c "$(cygpath -w "$VIVADO_BIN/xelab.bat") -L xpm work.video_source_sync_tb work.glbl -s tb_sync" > xelab_sync.log 2>&1 \
        || { cat xelab_sync.log; exit 1; }
    cmd //c "$(cygpath -w "$VIVADO_BIN/xelab.bat") -L xpm work.video_source_sync_formatter_tb work.glbl -s tb_fmt" > xelab_fmt.log 2>&1 \
        || { cat xelab_fmt.log; exit 1; }
else
    "$VIVADO_BIN/xvlog" dut.v video_source_sync.v video_overlay_pixel.v video_overlay_linebuffer.v "$HERE/video_source_sync_tb.v" "$HERE/video_source_sync_formatter_tb.v" "$GLBL" > xvlog.log 2>&1 \
        || { cat xvlog.log; exit 1; }
    "$VIVADO_BIN/xelab" -L xpm work.video_source_sync_tb work.glbl -s tb_sync > xelab_sync.log 2>&1 \
        || { cat xelab_sync.log; exit 1; }
    "$VIVADO_BIN/xelab" -L xpm work.video_source_sync_formatter_tb work.glbl -s tb_fmt > xelab_fmt.log 2>&1 \
        || { cat xelab_fmt.log; exit 1; }
fi

# The formatter integration testbench runs twice from the same
# elaborated snapshot: progressive (256 source rows, scale_y x4) and
# woven interlace (+INTERLACE=1: 512 woven rows, scale_y x2, one anchor
# per field, alternating short/long field cadence).  Both use the
# production 1080-line canvas with the 1024-row centered viewport at
# y=28.
if [ "$SMOKE" = "smoke" ]; then
    PLUS="--testplusarg \"SMOKE=1\""
    PLUS_I="--testplusarg \"SMOKE=1\" --testplusarg \"INTERLACE=1\""
else
    PLUS=""
    PLUS_I="--testplusarg \"INTERLACE=1\""
fi

rm -f run_sync.log run_fmt.log run_fmt_i.log
if [ "$ON_WINDOWS" = 1 ]; then
    cmd //c "$(cygpath -w "$VIVADO_BIN/xsim.bat") tb_sync --runall $PLUS" < /dev/null > run_sync.log 2>&1 || true
    cmd //c "$(cygpath -w "$VIVADO_BIN/xsim.bat") tb_fmt --runall $PLUS" < /dev/null > run_fmt.log 2>&1 || true
    cmd //c "$(cygpath -w "$VIVADO_BIN/xsim.bat") tb_fmt --runall $PLUS_I" < /dev/null > run_fmt_i.log 2>&1 || true
else
    # cmd needs quoted command strings; native xsim needs plain argv values.
    NATIVE_PLUS=()
    if [ "$SMOKE" = "smoke" ]; then
        NATIVE_PLUS=(--testplusarg SMOKE=1)
    fi
    "$VIVADO_BIN/xsim" tb_sync --runall "${NATIVE_PLUS[@]}" < /dev/null > run_sync.log 2>&1 || true
    "$VIVADO_BIN/xsim" tb_fmt --runall "${NATIVE_PLUS[@]}" < /dev/null > run_fmt.log 2>&1 || true
    "$VIVADO_BIN/xsim" tb_fmt --runall "${NATIVE_PLUS[@]}" --testplusarg INTERLACE=1 < /dev/null > run_fmt_i.log 2>&1 || true
fi

echo "---- controller (video_source_sync_tb) ----"
grep -E "SECTION|RESULT" run_sync.log | tail -30
echo "---- formatter integration, progressive (video_source_sync_formatter_tb) ----"
grep -E "PIXELS|MARGINS|CADENCE|RESULT" run_fmt.log
echo "---- formatter integration, woven interlace (+INTERLACE=1) ----"
grep -E "PIXELS|MARGINS|CADENCE|RESULT" run_fmt_i.log

RESULTS=0
grep -h "RESULT" run_sync.log run_fmt.log run_fmt_i.log 2>/dev/null | while read -r line; do echo "$line"; done
RESULTS=$(grep -h "RESULT" run_sync.log run_fmt.log run_fmt_i.log 2>/dev/null | wc -l)
if [ "$RESULTS" -ne 3 ]; then
    if grep -h -q "PrivateChannel: Error connecting to server socket" run_*.log 2>/dev/null; then
        echo "SIM: xsim localhost IPC failed; on Windows, stop Docker Desktop and retry"
    fi
    echo "SIM: MISSING RESULTS ($RESULTS of 3 testbench runs reported)"
    exit 1
fi
if grep -h "RESULT" run_sync.log run_fmt.log run_fmt_i.log | grep -qv "ERRORS=0"; then
    echo "SIM: FAILURES PRESENT"
    exit 1
fi
echo "SIM: ALL PASS"
