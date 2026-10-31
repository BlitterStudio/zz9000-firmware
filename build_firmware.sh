#!/bin/bash
# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Rebuild the ARM firmware (ZZ9000OS.elf).
#
# Requires arm-none-eabi-gcc on PATH (Arm GNU Toolchain with newlib).
# See ZZ9000_proto.sdk/ZZ9000OS/Makefile for toolchain notes.
#
# Output: ZZ9000_proto.sdk/ZZ9000OS/build/ZZ9000OS.elf
#
# Pass --rasterix to compile the firmware that is safe to package only with
# the qualified RasterIX bitstream.  The normal build contains no platform
# registration and therefore cannot issue a GP1 access to 0x83c60000.
# A --rasterix build streams command packets with the PL330 once its start-up
# self-test passes. Append MINIGL_RIX_DMA=0 to keep the MMIO path.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

MAKE_ARGS=("$@")
if [ "${1:-}" = "--rasterix" ]; then
    MAKE_ARGS=("MINIGL_RASTERIX=1" "${@:2}")
fi

if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    # Auto-discover common toolchain installs before giving up, so a fresh
    # terminal without Homebrew in PATH still works on macOS.
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

if ! command -v arm-none-eabi-objcopy >/dev/null 2>&1; then
    echo "ERROR: arm-none-eabi-objcopy not on PATH." >&2
    echo "  Install the full Arm GNU Toolchain and make sure its bin directory is on PATH." >&2
    exit 1
fi

if ! command -v "${CC_FOR_BUILD:-cc}" >/dev/null 2>&1; then
    echo "ERROR: host C compiler not found." >&2
    echo "  Set CC_FOR_BUILD=/path/to/cc, or install the platform C compiler." >&2
    exit 1
fi

echo "[firmware] toolchain: $(arm-none-eabi-gcc --version | head -1)"
echo "[firmware] host cc:   $(${CC_FOR_BUILD:-cc} --version 2>/dev/null | head -1 || printf '%s' "${CC_FOR_BUILD:-cc}")"

if [ "${#MAKE_ARGS[@]}" -eq 0 ] || [ "${MAKE_ARGS[0]}" != "clean" ] || [ "${#MAKE_ARGS[@]}" -gt 1 ]; then
    bash ./build_libjpeg_turbo.sh
    bash ./build_libpng.sh
    bash ./build_lzma_sdk.sh
    bash ./build_libwebp.sh
    bash ./build_dvd_codecs.sh --arm
    bash ./build_flac.sh --arm
    bash ./build_vorbis.sh --arm
    bash ./build_libvpx.sh --arm
    bash ./build_opus.sh --arm
fi

make -C ZZ9000_proto.sdk/ZZ9000OS "${MAKE_ARGS[@]}"

# Only report size when the ELF actually exists (skips cases like
# `./build_firmware.sh clean` where the ELF was just removed).
ELF=ZZ9000_proto.sdk/ZZ9000OS/build/ZZ9000OS.elf
if [ -f "$ELF" ]; then
    arm-none-eabi-size "$ELF"
    echo "[firmware] done: $ELF"
    bash ./util/check_elf_vectors.sh "$ELF"
else
    echo "[firmware] done (no ELF produced — target was likely 'clean')."
fi

FALLBACK_ELF=ZZ9000_proto.sdk/ZZ9000OS/build/ZZ9000OS-legacy-bitstream.elf
if [ -f "$FALLBACK_ELF" ]; then
    arm-none-eabi-size "$FALLBACK_ELF"
    echo "[firmware] legacy-bitstream fallback: $FALLBACK_ELF"
    bash ./util/check_elf_vectors.sh "$FALLBACK_ELF"
fi
