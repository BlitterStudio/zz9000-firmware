#!/bin/bash
# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build libwebp's decoder and demuxer as static archives for ZZ9000OS.
# The pinned source archive and all derived files stay below build/deps; no
# upstream source is vendored into the repository.

set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
OS_DIR="$SCRIPT_DIR/ZZ9000_proto.sdk/ZZ9000OS"
VERSION="${LIBWEBP_VERSION:-1.6.0}"
SHA256="${LIBWEBP_SHA256:-e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564}"
URL="${LIBWEBP_URL:-https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-${VERSION}.tar.gz}"
TARGET="${LIBWEBP_TARGET:-arm}"

usage() {
    cat <<'EOF'
Usage: build_libwebp.sh [--host|--arm]

Build libwebp 1.6.0 decoder and demux static archives. The default target is
ARM firmware; --host (or LIBWEBP_TARGET=host) builds native archives for
isolated image tests.
EOF
}

if [ "$#" -gt 1 ]; then
    usage >&2
    exit 2
fi
if [ "$#" -eq 1 ]; then
    case "$1" in
        --host) TARGET=host ;;
        --arm) TARGET=arm ;;
        --help|-h) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
fi
case "$TARGET" in
    arm|host) ;;
    *) echo "ERROR: LIBWEBP_TARGET must be arm or host." >&2; exit 2 ;;
esac

DEPS_DIR="$OS_DIR/build/deps/libwebp"
VERSION_DIR="$DEPS_DIR/$VERSION"
ARCHIVE="$VERSION_DIR/libwebp-${VERSION}.tar.gz"
SRC_DIR="$VERSION_DIR/src"
BUILD_DIR="$VERSION_DIR/$TARGET/build"
LIB_DIR="$VERSION_DIR/$TARGET/lib"
INCLUDE_DIR="$VERSION_DIR/$TARGET/include"
SHIM_DIR="$OS_DIR/src/webp"
ALLOC_HEADER="$OS_DIR/src/sdk_webp_alloc.h"
DECODER_LIB="$LIB_DIR/libwebpdecoder.a"
DEMUX_LIB="$LIB_DIR/libwebpdemux.a"

if [ ! -f "$SHIM_DIR/CMakeLists.txt" ] || [ ! -f "$ALLOC_HEADER" ]; then
    echo "ERROR: WebP build shim or allocator header is missing." >&2
    exit 1
fi

mkdir -p "$VERSION_DIR"
if [ ! -f "$ARCHIVE" ]; then
    echo "[libwebp] downloading $URL"
    if command -v curl >/dev/null 2>&1; then
        curl --fail --location --silent --show-error "$URL" -o "$ARCHIVE"
    elif command -v wget >/dev/null 2>&1; then
        wget -q "$URL" -O "$ARCHIVE"
    else
        echo "ERROR: install curl or wget to download libwebp." >&2
        exit 1
    fi
fi

if command -v sha256sum >/dev/null 2>&1; then
    ACTUAL_SHA256="$(sha256sum "$ARCHIVE" | awk '{print $1}')"
else
    ACTUAL_SHA256="$(shasum -a 256 "$ARCHIVE" | awk '{print $1}')"
fi
if [ "$ACTUAL_SHA256" != "$SHA256" ]; then
    echo "ERROR: libwebp archive checksum mismatch." >&2
    echo "  expected: $SHA256" >&2
    echo "  actual:   $ACTUAL_SHA256" >&2
    rm -f "$ARCHIVE"
    exit 1
fi

if [ ! -f "$SRC_DIR/CMakeLists.txt" ]; then
    rm -rf "$SRC_DIR"
    mkdir -p "$SRC_DIR"
    tar xzf "$ARCHIVE" --strip-components=1 -C "$SRC_DIR"
fi

if [ "$TARGET" = arm ]; then
    : "${ARM_NONE_EABI_CC:=arm-none-eabi-gcc}"
    : "${ARM_NONE_EABI_AR:=arm-none-eabi-ar}"
    : "${ARM_NONE_EABI_RANLIB:=arm-none-eabi-ranlib}"
    for tool in "$ARM_NONE_EABI_CC" "$ARM_NONE_EABI_AR" "$ARM_NONE_EABI_RANLIB"; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "ERROR: required ARM tool not found: $tool" >&2
            exit 1
        fi
    done
    CMAKE_TARGET_ARGS=(
        -DCMAKE_SYSTEM_NAME=Generic
        -DCMAKE_SYSTEM_PROCESSOR=arm
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
        "-DCMAKE_C_COMPILER=$(command -v "$ARM_NONE_EABI_CC")"
        "-DCMAKE_AR=$(command -v "$ARM_NONE_EABI_AR")"
        "-DCMAKE_RANLIB=$(command -v "$ARM_NONE_EABI_RANLIB")"
        "-DCMAKE_C_FLAGS=-mcpu=cortex-a9 -marm -mfpu=neon -mfloat-abi=hard -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-strict-aliasing"
    )
else
    : "${CC_FOR_BUILD:=cc}"
    : "${AR_FOR_BUILD:=ar}"
    : "${RANLIB_FOR_BUILD:=ranlib}"
    for tool in "$CC_FOR_BUILD" "$AR_FOR_BUILD" "$RANLIB_FOR_BUILD"; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "ERROR: required host tool not found: $tool" >&2
            exit 1
        fi
    done
    CMAKE_TARGET_ARGS=(
        "-DCMAKE_C_COMPILER=$(command -v "$CC_FOR_BUILD")"
        "-DCMAKE_AR=$(command -v "$AR_FOR_BUILD")"
        "-DCMAKE_RANLIB=$(command -v "$RANLIB_FOR_BUILD")"
        "-DCMAKE_C_FLAGS=-ffunction-sections -fdata-sections -fno-strict-aliasing"
    )
fi

mkdir -p "$BUILD_DIR"
echo "[libwebp] configuring decoder/demux ($TARGET)"
cmake -S "$SHIM_DIR" -B "$BUILD_DIR" \
    "${CMAKE_TARGET_ARGS[@]}" \
    "-DLIBWEBP_SOURCE_DIR=$SRC_DIR" \
    "-DSDK_WEBP_ALLOC_HEADER=$ALLOC_HEADER" \
    -DWEBP_ENABLE_SIMD=ON \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --target zz_webp_decoder_demux

mkdir -p "$LIB_DIR" "$INCLUDE_DIR"
cp "$BUILD_DIR/libwebp/libwebpdecoder.a" "$DECODER_LIB"
cp "$BUILD_DIR/libwebpdemux.a" "$DEMUX_LIB"
rm -rf "$INCLUDE_DIR/webp"
cp -R "$SRC_DIR/src/webp" "$INCLUDE_DIR/webp"

echo "[libwebp] built ($TARGET): $DECODER_LIB $DEMUX_LIB"
