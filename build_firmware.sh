#!/bin/bash
#
# Rebuild the ARM firmware (ZZ9000OS.elf).
#
# Requires arm-none-eabi-gcc on PATH (Arm GNU Toolchain with newlib).
# See ZZ9000_proto.sdk/ZZ9000OS/Makefile for toolchain notes.
#
# Output: ZZ9000_proto.sdk/ZZ9000OS/build/ZZ9000OS.elf

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    echo "ERROR: arm-none-eabi-gcc not on PATH." >&2
    echo "  macOS: brew install --cask gcc-arm-embedded" >&2
    echo "  Linux: install the official Arm GNU Toolchain (NOT Debian's gcc-arm-none-eabi," >&2
    echo "         which uses picolibc and is incompatible with the Xilinx BSP)." >&2
    exit 1
fi

echo "[firmware] toolchain: $(arm-none-eabi-gcc --version | head -1)"
make -C ZZ9000_proto.sdk/ZZ9000OS "$@"
arm-none-eabi-size ZZ9000_proto.sdk/ZZ9000OS/build/ZZ9000OS.elf
echo "[firmware] done: ZZ9000_proto.sdk/ZZ9000OS/build/ZZ9000OS.elf"
