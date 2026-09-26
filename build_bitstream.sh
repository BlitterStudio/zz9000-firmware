#!/bin/bash
#
# Rebuild the FPGA bitstream via Vivado 2018.3.
#
# Inputs:  mntzorro.v, video_formatter.v, zz9000_project.tcl, ip_repo/, etc.
# Output:  bootimage_work/zz9000_ps_wrapper.bit by default; C28 candidates
#          use bootimage_work/capture-c28/zz9000_ps_wrapper.bit.
#
# Requires a Linux host with Vivado 2018.3 installed. Default path is
# /opt/Xilinx/Vivado/2018.3; override with $VIVADO_DIR.
#
# After this, run ./build_firmware.sh (if firmware not yet built) and
# ./build_bootimage.sh --bitstream <output> to produce the final BOOT.bin.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

usage() {
    cat >&2 <<'EOF'
Usage: ./build_bitstream.sh [--no-autoboot] [--capture-c28] [--output PATH]

Options:
  --no-autoboot  Synthesize a diagnostic bitstream that does not advertise
                 the Zorro autoboot ROM.
  --capture-c28  Use the A4000 video-slot C28 clock, not the E7M clock.
  --output PATH  Write the generated bitstream to PATH.
EOF
}

PROJECT_ARGS=(--origin_dir .)
NO_AUTOBOOT=0
CAPTURE_C28=0
OUTPUT=

while [ "$#" -gt 0 ]; do
    case "$1" in
        --no-autoboot)
            NO_AUTOBOOT=1
            PROJECT_ARGS+=(--no-autoboot)
            shift
            ;;
        --capture-c28)
            CAPTURE_C28=1
            PROJECT_ARGS+=(--capture-c28)
            shift
            ;;
        --output)
            if [ "$#" -lt 2 ]; then
                echo "ERROR: --output needs a path." >&2
                exit 1
            fi
            OUTPUT=$2
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "ERROR: unknown argument: $1" >&2
            usage
            exit 1
            ;;
    esac
done

if [ -z "$OUTPUT" ]; then
    if [ "$CAPTURE_C28" -eq 1 ]; then
        OUTPUT=bootimage_work/capture-c28/zz9000_ps_wrapper.bit
    else
        OUTPUT=bootimage_work/zz9000_ps_wrapper.bit
    fi
fi
if [ "$CAPTURE_C28" -eq 1 ] &&
   [ "$(realpath -m "$OUTPUT")" = "$(realpath -m bootimage_work/zz9000_ps_wrapper.bit)" ]; then
    echo "ERROR: a C28 bitstream cannot replace the default E7M bitstream." >&2
    exit 1
fi

VIVADO_DIR="${VIVADO_DIR:-/opt/Xilinx/Vivado/2018.3}"

if [ ! -f "$VIVADO_DIR/settings64.sh" ]; then
    echo "ERROR: Vivado not found at $VIVADO_DIR" >&2
    echo "  Set VIVADO_DIR=/path/to/Vivado/2018.3 if installed elsewhere." >&2
    exit 1
fi

echo "[bitstream] Vivado: $VIVADO_DIR"
if [ "$NO_AUTOBOOT" -eq 1 ]; then
    echo "[bitstream] autoboot ROM: disabled"
fi
if [ "$CAPTURE_C28" -eq 1 ]; then
    echo "[bitstream] native capture clock: A4000 C28"
fi
# shellcheck disable=SC1091
source "$VIVADO_DIR/settings64.sh"

# 1. Regenerate the project from the TCL description — this is the only
#    reliable way to pick up changes to zz9000_project.tcl (e.g. new
#    block-design wiring). Keeping a stale ZZ9000_proto/ around has
#    historically caused silent failures.
echo "[bitstream] regenerating project from zz9000_project.tcl"
rm -rf ZZ9000_proto
vivado -mode batch -source zz9000_project.tcl -tclargs "${PROJECT_ARGS[@]}"

# 2. Synthesise → implement → write bitstream (15-30 min on a decent box).
echo "[bitstream] running synthesis + implementation + write_bitstream"
vivado -mode batch -source build_run_synthesis.tcl

# 3. Copy the fresh .bit into the selected release or diagnostic path.
BITSTREAM=$(find ZZ9000_proto -name "zz9000_ps_wrapper.bit" -path "*/impl_1/*" | head -1)
if [ -z "$BITSTREAM" ] || [ ! -f "$BITSTREAM" ]; then
    echo "ERROR: bitstream not produced — check the Vivado logs in" >&2
    echo "  ZZ9000_proto/ZZ9000_proto.runs/impl_1/runme.log" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUTPUT")"
cp "$BITSTREAM" "$OUTPUT"
echo "[bitstream] done: $OUTPUT"
if [ "$OUTPUT" = bootimage_work/zz9000_ps_wrapper.bit ]; then
    echo "[bitstream] NB: commit bootimage_work/zz9000_ps_wrapper.bit so CI"
    echo "[bitstream]     (which can't run Vivado) picks up your HDL changes."
fi
