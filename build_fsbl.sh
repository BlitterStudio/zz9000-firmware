#!/bin/bash
# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Rebuild the first-stage bootloader from source.
#
# Requires arm-none-eabi-gcc on PATH (Arm GNU Toolchain with newlib).
# See ZZ9000_proto.sdk/ZZ9000OS/Makefile for toolchain notes.
#
# Output: ZZ9000_proto.sdk/ZZ9000FSBL/build/ZZ9000FSBL.elf
#
# --install copies the result over bootimage_work/FSBL_exec.elf, the
# committed artifact build_bootimage.sh packages into BOOT.bin. Do NOT
# install until the rebuilt FSBL has been validated against the old one
# and bench-tested with a recovery SD card at hand — a broken FSBL means
# the board does not boot at all. Full procedure:
# ZZ9000_proto.sdk/ZZ9000FSBL/README.md

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

INSTALL=0
MAKE_TARGET=all
while [ "$#" -gt 0 ]; do
    case "$1" in
        --install) INSTALL=1 ;;
        clean)     MAKE_TARGET=clean ;;
        -h|--help)
            echo "Usage: ./build_fsbl.sh [clean] [--install]"
            exit 0
            ;;
        *)
            echo "ERROR: unknown argument: $1" >&2
            exit 1
            ;;
    esac
    shift
done

if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    for candidate in \
        /opt/homebrew/bin \
        /usr/local/bin \
        /Applications/ArmGNUToolchain/*/*/bin; do
        if [ -x "$candidate/arm-none-eabi-gcc" ]; then
            PATH="$candidate:$PATH"
            break
        fi
    done
fi

if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    echo "ERROR: arm-none-eabi-gcc not on PATH." >&2
    echo "  macOS: brew install --cask gcc-arm-embedded" >&2
    echo "  Linux: install the official Arm GNU Toolchain (NOT Debian's gcc-arm-none-eabi," >&2
    echo "         which uses picolibc and is incompatible with the Xilinx BSP)." >&2
    exit 1
fi

echo "[fsbl] toolchain: $(arm-none-eabi-gcc --version | head -1)"

if [ "$MAKE_TARGET" = all ]; then
    bash ./util/refresh_ps7_init.sh --check
fi

make -C ZZ9000_proto.sdk/ZZ9000FSBL "$MAKE_TARGET"

ELF=ZZ9000_proto.sdk/ZZ9000FSBL/build/ZZ9000FSBL.elf
if [ "$MAKE_TARGET" = clean ]; then
    echo "[fsbl] done (clean)."
    exit 0
fi

if [ "$INSTALL" = 1 ]; then
    cp "$ELF" bootimage_work/FSBL_exec.elf
    echo "[fsbl] installed to bootimage_work/FSBL_exec.elf"
fi

echo "[fsbl] done: $ELF"
