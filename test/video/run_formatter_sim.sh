#!/bin/bash
# Functional simulation of video_formatter.v with Vivado xsim.
# Usage: ./run_formatter_sim.sh [master|current]
#   master  - runs the testbench against the pre-branch 32-bit formatter
#             (validates the testbench reference model)
#   current - runs against the working-tree video_formatter.v (default)
set -e

VIVADO_BIN="${VIVADO_BIN:-D:/Xilinx/Vivado/2018.3/bin}"
VARIANT="${1:-current}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SIMDIR="$HERE/build/sim_$VARIANT"

mkdir -p "$SIMDIR"
cd "$SIMDIR"

if [ "$VARIANT" = "master" ]; then
    git -C "$ROOT" show master:video_formatter.v > dut.v
    DEFINE="-d MASTER_DUT"
else
    cp "$ROOT/video_formatter.v" dut.v
    DEFINE=""
fi

cmd //c "$(cygpath -w "$VIVADO_BIN/xvlog.bat") $DEFINE dut.v $(cygpath -w "$HERE/video_formatter_tb.v")" > xvlog.log 2>&1 \
    || { cat xvlog.log; exit 1; }
cmd //c "$(cygpath -w "$VIVADO_BIN/xelab.bat") -L xpm work.video_formatter_tb -s tb" > xelab.log 2>&1 \
    || { cat xelab.log; exit 1; }

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
    cmd //c "$(cygpath -w "$VIVADO_BIN/xsim.bat") tb --runall --testplusarg \"CMODE=$CM\" --testplusarg \"SCALEX=$SX\" --testplusarg \"SCALEY=$SY\" --testplusarg \"WIDTH=$W\"" \
        > "run_${CM}_${SX}_${SY}_${W}.log" 2>&1 || true
    grep -E "RESULT|MISMATCH row=[0-9]+ x=(0|1|2|3|4|5|6|7) " "run_${CM}_${SX}_${SY}_${W}.log" | head -8
done

echo "---- summary ($VARIANT) ----"
grep -h "RESULT" run_*.log
RESULTS=$(grep -h "RESULT" run_*.log | wc -l)
if [ "$RESULTS" -ne "$EXPECTED" ]; then
    # a run that crashes before the testbench prints RESULT must not be
    # silently dropped from the verdict
    echo "SIM: MISSING RESULTS ($RESULTS of $EXPECTED configs reported)"
    exit 1
fi
if grep -h "RESULT" run_*.log | grep -qv "MISMATCHES=0"; then
    echo "SIM: FAILURES PRESENT"
    exit 1
fi
echo "SIM: ALL PASS ($RESULTS/$EXPECTED configs)"
