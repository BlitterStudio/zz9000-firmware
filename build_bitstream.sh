#!/bin/bash
#
# Rebuild the FPGA bitstream via Vivado 2018.3.
#
# Inputs:  mntzorro.v, video_formatter.v, zz9000_project.tcl, ip_repo/, etc.
# Output:  bootimage_work/zz9000_ps_wrapper.bit  (copied from the Vivado run dir)
#
# Requires a Linux host with Vivado 2018.3 installed. Default path is
# /opt/Xilinx/Vivado/2018.3; override with $VIVADO_DIR.
#
# After this, run ./build_firmware.sh (if firmware not yet built) and
# ./build_bootimage.sh to produce the final BOOT.bin.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

VIVADO_DIR="${VIVADO_DIR:-/opt/Xilinx/Vivado/2018.3}"

if [ ! -f "$VIVADO_DIR/settings64.sh" ]; then
    echo "ERROR: Vivado not found at $VIVADO_DIR" >&2
    echo "  Set VIVADO_DIR=/path/to/Vivado/2018.3 if installed elsewhere." >&2
    exit 1
fi

echo "[bitstream] Vivado: $VIVADO_DIR"
# shellcheck disable=SC1091
source "$VIVADO_DIR/settings64.sh"

# 1. Regenerate the project from the TCL description — this is the only
#    reliable way to pick up changes to zz9000_project.tcl (e.g. new
#    block-design wiring). Keeping a stale ZZ9000_proto/ around has
#    historically caused silent failures.
echo "[bitstream] regenerating project from zz9000_project.tcl"
rm -rf ZZ9000_proto
vivado -mode batch -source zz9000_project.tcl -tclargs --origin_dir .

# 2. Synthesise → implement → write bitstream (15-30 min on a decent box).
echo "[bitstream] running synthesis + implementation + write_bitstream"
vivado -mode batch -source build_run_synthesis.tcl

# 3. Copy the fresh .bit into bootimage_work/ so build_bootimage.sh picks
#    it up. This is the same file CI uses when it can't run Vivado itself,
#    so committing the updated .bit is how HDL changes reach CI builds.
BITSTREAM=$(find ZZ9000_proto -name "zz9000_ps_wrapper.bit" -path "*/impl_1/*" | head -1)
if [ -z "$BITSTREAM" ] || [ ! -f "$BITSTREAM" ]; then
    echo "ERROR: bitstream not produced — check the Vivado logs in" >&2
    echo "  ZZ9000_proto/ZZ9000_proto.runs/impl_1/runme.log" >&2
    exit 1
fi

cp "$BITSTREAM" bootimage_work/zz9000_ps_wrapper.bit
echo "[bitstream] done: bootimage_work/zz9000_ps_wrapper.bit"
echo "[bitstream] NB: commit bootimage_work/zz9000_ps_wrapper.bit so CI"
echo "[bitstream]     (which can't run Vivado) picks up your HDL changes."
