#!/bin/bash
set -e

# ZZ9000 Bitstream Build Script
# Run on x86_64 Linux with Vivado 2018.3 installed
#
# Usage:
#   1. Copy the entire zz9000-firmware directory to your Linux machine
#   2. Run: bash build_bitstream.sh [/path/to/Vivado/2018.3] [/path/to/SDK/2018.3]
#   3. Copy the resulting BOOT.bin back to ZZ9000 MicroSD

VIVADO_DIR="${1:-/opt/Xilinx/Vivado/2018.3}"
SDK_DIR="${2:-/opt/Xilinx/SDK/2018.3}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

cd "$SCRIPT_DIR"

if [ ! -f "$VIVADO_DIR/settings64.sh" ]; then
    echo "ERROR: Vivado not found at $VIVADO_DIR"
    echo "Usage: $0 [/path/to/Vivado/2018.3] [/path/to/SDK/2018.3]"
    exit 1
fi

echo "================================================"
echo " ZZ9000 Bitstream Build"
echo " Vivado: $VIVADO_DIR"
echo " SDK:    $SDK_DIR"
echo "================================================"
echo ""

# Step 1: Source Vivado and create project
echo "[1/4] Creating Vivado project..."
source "$VIVADO_DIR/settings64.sh"
rm -rf ZZ9000_proto
vivado -mode batch -source zz9000_project.tcl -tclargs --origin_dir .
echo "  Done."

# Step 2: Synthesize, implement, generate bitstream
echo "[2/4] Running synthesis + implementation + bitstream (this takes 15-30 min)..."
vivado -mode batch -source build_run_synthesis.tcl
echo "  Done."

# Step 3: Generate FSBL
echo "[3/4] Generating FSBL..."
FSBL_ELF=""
if [ -f "$SDK_DIR/bin/xsct" ]; then
    source "$SDK_DIR/settings64.sh" 2>/dev/null || true
    cat > _gen_fsbl.tcl << 'TCLEOF'
setws ZZ9000_proto/ZZ9000_proto.sdk
createhw -name hw_platform -hwspec ZZ9000_proto/ZZ9000_proto.sdk/zz9000_ps_wrapper.hdf
createapp -name ZZ9000FSBL -hwproject hw_platform -proc ps7_cortexa9_0 -os standalone -lang C -app {Zynq FSBL}
projects -build
TCLEOF
    xsct _gen_fsbl.tcl
    FSBL_ELF="ZZ9000_proto/ZZ9000_proto.sdk/ZZ9000FSBL/Debug/ZZ9000FSBL.elf"
    echo "  FSBL generated: $FSBL_ELF"
else
    echo "  WARNING: Xilinx SDK (xsct) not found at $SDK_DIR"
    echo "  Generate FSBL manually in Xilinx SDK, or use the pre-built one."
fi

# Step 4: Package BOOT.bin
echo "[4/4] Packaging BOOT.bin..."
BITSTREAM=$(find ZZ9000_proto -name "zz9000_ps_wrapper.bit" -path "*/impl_1/*" | head -1)
ZZ9000OS_ELF="ZZ9000_proto.sdk/ZZ9000OS/build/ZZ9000OS.elf"

if [ -z "$FSBL_ELF" ] || [ ! -f "$FSBL_ELF" ]; then
    echo "  FSBL not found via SDK. Checking for pre-built..."
    FSBL_ELF=$(find ZZ9000_proto -name "*FSBL*.elf" 2>/dev/null | head -1)
fi

echo "  FSBL:      ${FSBL_ELF:-MISSING}"
echo "  Bitstream: ${BITSTREAM:-MISSING}"
echo "  ZZ9000OS:  ${ZZ9000OS_ELF}"

if [ -f "$FSBL_ELF" ] && [ -f "$BITSTREAM" ] && [ -f "$ZZ9000OS_ELF" ]; then
    cat > _boot.bif << EOF
the_ROM_image:
{
  [bootloader] $FSBL_ELF
  $BITSTREAM
  $ZZ9000OS_ELF
}
EOF
    bootgen -image _boot.bif -arch zynq -w on -o BOOT.bin
    echo ""
    echo "================================================"
    echo " SUCCESS!"
    echo " BOOT.bin: $(pwd)/BOOT.bin"
    echo " Size: $(stat -c%s BOOT.bin 2>/dev/null || stat -f%z BOOT.bin) bytes"
    echo "================================================"
    echo ""
    echo " Copy BOOT.bin to ZZ9000 MicroSD"
else
    echo ""
    echo "================================================"
    echo " PARTIAL SUCCESS - bitstream built but BOOT.bin packaging incomplete"
    echo "================================================"
    echo ""
    if [ -f "$BITSTREAM" ]; then
        echo " Bitstream: $BITSTREAM"
        echo " Copy this bitstream to your Mac and use bootgen there"
        echo " with a valid FSBL.elf to create BOOT.bin"
    fi
fi

# Cleanup temp TCL files
rm -f _gen_fsbl.tcl _boot.bif

echo "Done."
