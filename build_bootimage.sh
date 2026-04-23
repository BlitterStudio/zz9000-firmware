#!/bin/bash
#
# Package BOOT.bin from the committed bootimage.bif.
#
# Inputs:
#   bootimage_work/FSBL_exec.elf              (committed)
#   bootimage_work/zz9000_ps_wrapper.bit      (committed or rebuilt by build_bitstream.sh)
#   ZZ9000_proto.sdk/ZZ9000OS/build/ZZ9000OS.elf  (rebuilt by build_firmware.sh)
#
# Output: bootimage_work/BOOT.bin
#
# Bootgen discovery order:
#   1. $BOOTGEN (explicit override)
#   2. bootgen on PATH
#   3. /Users/midwan/Gitlab/bootgen/bootgen (Mac default — adjust if you moved it)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Locate bootgen. In-tree bootgen/ is the usual build location from the
# sibling bootgen repo; fall back to an older out-of-tree spot for
# historical macOS setups.
if [ -n "${BOOTGEN:-}" ]; then
    : # use explicit override
elif command -v bootgen >/dev/null 2>&1; then
    BOOTGEN=$(command -v bootgen)
elif [ -x "$SCRIPT_DIR/bootgen/bootgen" ]; then
    BOOTGEN="$SCRIPT_DIR/bootgen/bootgen"
elif [ -x /Users/midwan/Gitlab/bootgen/bootgen ]; then
    BOOTGEN=/Users/midwan/Gitlab/bootgen/bootgen
else
    echo "ERROR: bootgen not found." >&2
    echo "  Set BOOTGEN=/path/to/bootgen, install it on PATH, or build from" >&2
    echo "  https://github.com/Xilinx/bootgen and place the binary somewhere reachable." >&2
    exit 1
fi

# Sanity-check required inputs exist.
required=(
    bootimage_work/bootimage.bif
    bootimage_work/FSBL_exec.elf
    bootimage_work/zz9000_ps_wrapper.bit
    ZZ9000_proto.sdk/ZZ9000OS/build/ZZ9000OS.elf
)
for f in "${required[@]}"; do
    if [ ! -f "$f" ]; then
        echo "ERROR: missing required input: $f" >&2
        if [ "$f" = "ZZ9000_proto.sdk/ZZ9000OS/build/ZZ9000OS.elf" ]; then
            echo "  Run ./build_firmware.sh first." >&2
        fi
        exit 1
    fi
done

echo "[bootimage] bootgen: $BOOTGEN"
echo "[bootimage] bif:     bootimage_work/bootimage.bif"

rm -f bootimage_work/BOOT.bin
"$BOOTGEN" -arch zynq -image bootimage_work/bootimage.bif -w on -o bootimage_work/BOOT.bin

ls -lh bootimage_work/BOOT.bin
echo "[bootimage] done: bootimage_work/BOOT.bin"
