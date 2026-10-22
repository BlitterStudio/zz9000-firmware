#!/bin/bash
# Focused simulation of the registered, source-synchronous VGA output stage.
set -e

if command -v cygpath >/dev/null 2>&1; then
    ON_WINDOWS=1
    VIVADO_BIN="${VIVADO_BIN:-D:/Xilinx/Vivado/2018.3/bin}"
else
    ON_WINDOWS=0
    VIVADO_BIN="${VIVADO_BIN:-/opt/Xilinx/Vivado/2018.3/bin}"
fi
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SIMDIR="$HERE/build/sim_output_stage"
GLBL="$VIVADO_BIN/../data/verilog/src/glbl.v"

rm -rf "$SIMDIR"
mkdir -p "$SIMDIR"
cd "$SIMDIR"
cp "$ROOT/vga_output_stage.v" "$HERE/vga_output_stage_tb.v" .

if [ "$ON_WINDOWS" = 1 ]; then
    MSYS_NO_PATHCONV=1 cmd.exe /c "$(cygpath -w "$VIVADO_BIN/xvlog.bat") $(cygpath -w vga_output_stage.v) $(cygpath -w vga_output_stage_tb.v) $(cygpath -w "$GLBL")"
    MSYS_NO_PATHCONV=1 cmd.exe /c "$(cygpath -w "$VIVADO_BIN/xelab.bat") -L unisim work.vga_output_stage_tb work.glbl -s vga_output_stage_tb"
    MSYS_NO_PATHCONV=1 cmd.exe /c "$(cygpath -w "$VIVADO_BIN/xsim.bat") vga_output_stage_tb --runall"
else
    "$VIVADO_BIN/xvlog" vga_output_stage.v "$HERE/vga_output_stage_tb.v" "$GLBL"
    "$VIVADO_BIN/xelab" -L unisim work.vga_output_stage_tb work.glbl -s vga_output_stage_tb
    "$VIVADO_BIN/xsim" vga_output_stage_tb --runall
fi
