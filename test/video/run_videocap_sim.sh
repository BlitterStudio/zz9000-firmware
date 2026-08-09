#!/bin/bash
# Functional simulation of videocap_sampler.v with Vivado xsim.
# Usage: ./run_videocap_sim.sh
set -e

# Windows Git Bash drives the .bat tools through cmd. Linux runs the Vivado
# binaries directly.
if command -v cygpath >/dev/null 2>&1; then
    ON_WINDOWS=1
    VIVADO_BIN="${VIVADO_BIN:-D:/Xilinx/Vivado/2018.3/bin}"
else
    ON_WINDOWS=0
    VIVADO_BIN="${VIVADO_BIN:-/opt/Xilinx/Vivado/2018.3/bin}"
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SIMDIR="$HERE/build/sim_videocap"
GLBL="$VIVADO_BIN/../data/verilog/src/glbl.v"

mkdir -p "$SIMDIR"
cd "$SIMDIR"
cp "$ROOT/videocap_sampler.v" .

if [ "$ON_WINDOWS" = 1 ]; then
    TB="$(cygpath -w "$HERE/videocap_sampler_tb.v")"
    cmd //c "$(cygpath -w "$VIVADO_BIN/xvlog.bat") videocap_sampler.v $TB $(cygpath -w "$GLBL")" > xvlog.log 2>&1 \
        || { cat xvlog.log; exit 1; }
    cmd //c "$(cygpath -w "$VIVADO_BIN/xelab.bat") -L xpm work.videocap_sampler_tb work.glbl -s tb" > xelab.log 2>&1 \
        || { cat xelab.log; exit 1; }
else
    "$VIVADO_BIN/xvlog" videocap_sampler.v "$HERE/videocap_sampler_tb.v" "$GLBL" > xvlog.log 2>&1 \
        || { cat xvlog.log; exit 1; }
    "$VIVADO_BIN/xelab" -L xpm work.videocap_sampler_tb work.glbl -s tb > xelab.log 2>&1 \
        || { cat xelab.log; exit 1; }
fi

# PIXSPAN SAMPLEMODE FULLWIDTH CROPH CROPV
CONFIGS="
4 0 0 188 26
2 0 0 188 26
2 0 0 288 26
2 0 0 188 40
1 0 0 188 26
4 0 1 188 26
2 0 1 188 26
1 0 1 188 26
1 0 1 288 40
1 1 0 188 26
1 2 0 188 26
2 1 0 188 26
2 2 0 188 26
"

rm -f run_*.log
EXPECTED=$(echo "$CONFIGS" | grep -c '[0-9]')
echo "$CONFIGS" | while read -r PS SM FW CH CV; do
    [ -z "$PS" ] && continue
    LOG="run_${PS}_${SM}_${FW}_${CH}_${CV}.log"
    if [ "$ON_WINDOWS" = 1 ]; then
        cmd //c "$(cygpath -w "$VIVADO_BIN/xsim.bat") tb --runall --testplusarg \"PIXSPAN=$PS\" --testplusarg \"SAMPLEMODE=$SM\" --testplusarg \"FULLWIDTH=$FW\" --testplusarg \"CROPH=$CH\" --testplusarg \"CROPV=$CV\"" \
            < /dev/null > "$LOG" 2>&1 || true
    else
        "$VIVADO_BIN/xsim" tb --runall --testplusarg "PIXSPAN=$PS" --testplusarg "SAMPLEMODE=$SM" --testplusarg "FULLWIDTH=$FW" --testplusarg "CROPH=$CH" --testplusarg "CROPV=$CV" \
            < /dev/null > "$LOG" 2>&1 || true
    fi
    grep -E "RESULT|MISMATCH" "$LOG" | head -8 || true
done

echo "---- summary ----"
grep -h "RESULT" run_*.log || true
RESULTS=$(grep -h "RESULT" run_*.log 2>/dev/null | wc -l)
if [ "$RESULTS" -ne "$EXPECTED" ]; then
    if grep -h -q "PrivateChannel: Error connecting to server socket" run_*.log 2>/dev/null; then
        echo "SIM: xsim localhost IPC failed; on Windows, stop Docker Desktop and retry"
    fi
    echo "SIM: MISSING RESULTS ($RESULTS of $EXPECTED configs reported)"
    exit 1
fi
if grep -h "RESULT" run_*.log | grep -q "RESULT FAIL"; then
    echo "SIM: FAILURES PRESENT"
    exit 1
fi
echo "SIM: ALL PASS ($RESULTS/$EXPECTED configs)"
