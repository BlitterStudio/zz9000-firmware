#!/bin/bash
# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build libpng as a static ARM archive for firmware PNG decode.
#
# libpng depends on the static zlib archive built by build_zlib.sh. Both
# dependencies are cached under ZZ9000OS/build/deps.

set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
OS_DIR="$SCRIPT_DIR/ZZ9000_proto.sdk/ZZ9000OS"
VERSION="${LIBPNG_VERSION:-1.6.58}"
SHA256="${LIBPNG_SHA256:-28eb403f51f0f7405249132cecfe82ea5c0ef97f1b32c5a65828814ae0d34775}"
ZLIB_VERSION="${ZLIB_VERSION:-1.3.2}"
DEPS_DIR="$OS_DIR/build/deps/libpng"
VERSION_DIR="$DEPS_DIR/$VERSION"
SRC_DIR="$VERSION_DIR/src"
BUILD_DIR="$VERSION_DIR/build-neon"
ARCHIVE="$DEPS_DIR/libpng-$VERSION.tar.xz"
URL="https://download.sourceforge.net/libpng/libpng-$VERSION.tar.xz"
ZLIB_SRC="$OS_DIR/build/deps/zlib/$ZLIB_VERSION/src"
ZLIB_BUILD="$OS_DIR/build/deps/zlib/$ZLIB_VERSION/build"
ZLIB_LIB="$ZLIB_BUILD/libz.a"

for tool in arm-none-eabi-gcc arm-none-eabi-ar arm-none-eabi-ranlib cmake make wget; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "ERROR: $tool not found on PATH." >&2
    exit 1
  fi
done

if [ ! -f "$ZLIB_LIB" ]; then
  "$SCRIPT_DIR/build_zlib.sh"
fi

if [ -f "$BUILD_DIR/liblibpng16_static.a" ]; then
  echo "[libpng] already built: $BUILD_DIR/liblibpng16_static.a"
  exit 0
fi

mkdir -p "$DEPS_DIR" "$VERSION_DIR"

if [ ! -f "$ARCHIVE" ]; then
  echo "[libpng] downloading $URL"
  wget -q "$URL" -O "$ARCHIVE"
fi

if command -v sha256sum >/dev/null 2>&1; then
  ACTUAL_SHA256="$(sha256sum "$ARCHIVE" | awk '{print $1}')"
else
  ACTUAL_SHA256="$(shasum -a 256 "$ARCHIVE" | awk '{print $1}')"
fi
if [ "$ACTUAL_SHA256" != "$SHA256" ]; then
  echo "ERROR: libpng archive checksum mismatch." >&2
  echo "  expected: $SHA256" >&2
  echo "  actual:   $ACTUAL_SHA256" >&2
  rm -f "$ARCHIVE"
  exit 1
fi

if [ ! -f "$SRC_DIR/CMakeLists.txt" ]; then
  echo "[libpng] extracting source"
  rm -rf "$SRC_DIR"
  mkdir -p "$SRC_DIR"
  tar xJf "$ARCHIVE" --strip-components=1 -C "$SRC_DIR"
fi

mkdir -p "$BUILD_DIR"

echo "[libpng] configuring NEON static libpng"
cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
  -DCMAKE_SYSTEM_NAME=Generic \
  -DCMAKE_SYSTEM_PROCESSOR=arm \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_C_COMPILER=arm-none-eabi-gcc \
  -DCMAKE_AR=arm-none-eabi-ar \
  -DCMAKE_RANLIB=arm-none-eabi-ranlib \
  -DCMAKE_C_FLAGS="-mcpu=cortex-a9 -marm -mfpu=neon -mfloat-abi=hard -O2 -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-strict-aliasing" \
  -DPNG_TARGET_ARCHITECTURE=arm \
  -DPNG_ARM_NEON=on \
  -DPNG_SHARED=OFF \
  -DPNG_STATIC=ON \
  -DPNG_TESTS=OFF \
  -DPNG_TOOLS=OFF \
  -DPNG_EXECUTABLES=OFF \
  -DZLIB_INCLUDE_DIR="$ZLIB_SRC" \
  -DZLIB_LIBRARY="$ZLIB_LIB"

echo "[libpng] building png_static"
cmake --build "$BUILD_DIR" --target png_static --parallel
echo "[libpng] done: $BUILD_DIR/liblibpng16_static.a"
