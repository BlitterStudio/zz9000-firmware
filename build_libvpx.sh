#!/bin/bash
# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Decoder-only libvpx for the diag/webm-bench image. Not part of the
# release firmware. VP8 and VP9 encoders, high bitdepth, examples, tools
# and the multithreaded row/tile paths are left out: the bare-metal image
# has no pthreads, and the bench is single-threaded on purpose.
#
# armv7-linux-gcc is the target that selects the NEON assembly. Its
# default softfp ABI is rewritten to hard-float so the archive matches
# ZZ9000OS (-mfloat-abi=hard). Runtime CPU detection is off; this core
# always has NEON.

set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
OS_DIR="$SCRIPT_DIR/ZZ9000_proto.sdk/ZZ9000OS"
VERSION="${LIBVPX_VERSION:-1.17.0}"
# GitHub tag archive for v1.17.0 (2026-08-07, "Yellowbilled Duck").
SHA256="${LIBVPX_SHA256:-1020f184046187baa2985dbde38e0691f49c44088bca7a1842b0236c6081dc0a}"
URL="${LIBVPX_URL:-https://github.com/webmproject/libvpx/archive/refs/tags/v${VERSION}.tar.gz}"
TARGET="${LIBVPX_TARGET:-arm}"
DEPS_DIR="$OS_DIR/build/deps/vpx/${VERSION}"
SRC="$DEPS_DIR/src"
OUT="$DEPS_DIR/$TARGET"
LIB="$OUT/libvpxdec.a"

usage() {
    echo "Usage: build_libvpx.sh [--arm|--host]" >&2
}

case "${1:-}" in
    ""|--arm) TARGET=arm ;;
    --host) TARGET=host ;;
    -h|--help) usage; exit 0 ;;
    *) usage; exit 2 ;;
esac
OUT="$DEPS_DIR/$TARGET"
LIB="$OUT/libvpxdec.a"

fetch() {
    local dest="$1"
    mkdir -p "$(dirname "$dest")"
    if [ ! -f "$dest" ]; then
        echo "[vpx] downloading $URL"
        if command -v curl >/dev/null 2>&1; then
            curl --fail --location --silent --show-error "$URL" -o "$dest"
        else
            wget -q "$URL" -O "$dest"
        fi
    fi
    if command -v sha256sum >/dev/null 2>&1; then
        actual="$(sha256sum "$dest" | awk '{print $1}')"
    else
        actual="$(shasum -a 256 "$dest" | awk '{print $1}')"
    fi
    echo "[vpx] sha256 $actual"
    if [ "$actual" != "$SHA256" ]; then
        echo "ERROR: libvpx checksum mismatch" >&2
        echo "  expected: $LIBVPX_SHA256" >&2
        echo "  actual:   $actual" >&2
        exit 1
    fi
}

mkdir -p "$DEPS_DIR"
fetch "$DEPS_DIR/libvpx-${VERSION}.tar.gz"
if [ ! -f "$SRC/configure" ]; then
    rm -rf "$SRC"
    mkdir -p "$SRC"
    tar xzf "$DEPS_DIR/libvpx-${VERSION}.tar.gz" --strip-components=1 -C "$SRC"
fi

if [ -f "$LIB" ] && [ "$SRC/configure" -ot "$LIB" ]; then
    echo "[vpx] up to date: $LIB"
    exit 0
fi

BUILD="$DEPS_DIR/build-$TARGET"
rm -rf "$BUILD"
mkdir -p "$BUILD" "$OUT/include"
cd "$BUILD"

CFLAGS_COMMON="-O2 -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-strict-aliasing"

if [ "$TARGET" = arm ]; then
    : "${ARM_NONE_EABI_CC:=arm-none-eabi-gcc}"
    : "${ARM_NONE_EABI_AR:=arm-none-eabi-ar}"
    WRAP="$BUILD/wrap"
    mkdir -p "$WRAP"
    # configure links a throwaway executable. newlib has no _exit unless
    # nosys (or the firmware's Xilinx.spec) is on the link line.
    printf '%s\n' '#!/bin/sh' "exec $ARM_NONE_EABI_CC -specs=nosys.specs \"\$@\"" > "$WRAP/gcc"
    printf '%s\n' '#!/bin/sh' "exec $ARM_NONE_EABI_CC -c \"\$@\"" > "$WRAP/as"
    chmod +x "$WRAP/gcc" "$WRAP/as"
    # Scripts, not symlinks: the worktree is a Windows mount and Docker
    # will not execute a symlink there.
    printf '%s\n' '#!/bin/sh' "exec $ARM_NONE_EABI_AR \"\$@\"" > "$WRAP/ar"
    printf '%s\n' '#!/bin/sh' 'exec arm-none-eabi-ranlib "$@"' > "$WRAP/ranlib"
    printf '%s\n' '#!/bin/sh' 'exec arm-none-eabi-nm "$@"' > "$WRAP/nm"
    printf '%s\n' '#!/bin/sh' 'exec arm-none-eabi-strip "$@"' > "$WRAP/strip"
    chmod +x "$WRAP/gcc" "$WRAP/as" "$WRAP/ar" "$WRAP/ranlib" "$WRAP/nm" "$WRAP/strip"
    export CROSS="$WRAP/"
    unset CC AR AS
    # -mcpu=cortex-a9 conflicts with the target's -march=armv7-a under
    # -Werror. Tune instead, and rewrite the target's softfp ABI below.
    EXTRA="-mtune=cortex-a9 -marm -mfpu=neon $CFLAGS_COMMON -DSDK_VORBIS_REPLACE_ALLOCATORS -include $OS_DIR/src/sdk_vorbis_alloc.h"
    bash "$SRC/configure" \
        --target=armv7-linux-gcc \
        --prefix="$OUT" \
        --enable-vp8-decoder --enable-vp9-decoder \
        --disable-vp8-encoder --disable-vp9-encoder \
        --disable-multithread --disable-runtime-cpu-detect \
        --force-disable-os_support \
        --disable-vp9-highbitdepth --disable-webm-io --disable-libyuv \
        --disable-examples --disable-tools --disable-docs --disable-unit-tests \
        --disable-pic \
        --enable-static --disable-shared \
        --extra-cflags="$EXTRA"
    # The armv7-linux-gcc target injects softfp. Last -mfloat-abi wins only
    # if it follows that injection, so rewrite the generated fragments.
    find . -type f \( -name 'Makefile' -o -name '*.mk' \) -print0 |
        xargs -0 sed -i 's/-mfloat-abi=softfp/-mfloat-abi=hard/g'
else
    bash "$SRC/configure" \
        --prefix="$OUT" \
        --enable-vp8-decoder --enable-vp9-decoder \
        --disable-vp8-encoder --disable-vp9-encoder \
        --disable-multithread --disable-runtime-cpu-detect \
        --disable-vp9-highbitdepth --disable-webm-io --disable-libyuv \
        --disable-examples --disable-tools --disable-docs --disable-unit-tests \
        --enable-static --disable-shared \
        --extra-cflags="$CFLAGS_COMMON"
fi

if ! grep -q '#define CONFIG_MULTITHREAD 0' vpx_config.h; then
    echo "ERROR: libvpx built with multithreading still enabled" >&2
    exit 1
fi
if [ "$TARGET" = arm ] && ! grep -q '#define HAVE_NEON 1' vpx_config.h; then
    echo "ERROR: libvpx ARM build did not enable NEON" >&2
    exit 1
fi

make -j"$(nproc 2>/dev/null || echo 4)"
make install
cp -a libvpx.a "$LIB"
# vpx_integer.h includes vpx_config.h from the vpx/ directory.
if [ -f vpx_config.h ]; then
    mkdir -p "$OUT/include/vpx"
    cp -a vpx_config.h "$OUT/include/vpx/"
fi
echo "[vpx] built $LIB"
if [ "$TARGET" = arm ]; then
    arm-none-eabi-size -t "$LIB" | tail -1
    mkdir -p "$OS_DIR/src/webm_codecs"
    cp -a "$SRC/LICENSE" "$OS_DIR/src/webm_codecs/COPYING.libvpx"
    cp -a "$SRC/PATENTS" "$OS_DIR/src/webm_codecs/PATENTS.libvpx"
fi
