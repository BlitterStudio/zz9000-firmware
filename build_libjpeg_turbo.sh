#!/bin/bash
# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build the firmware JPEG dependency as a static libjpeg-turbo archive.
#
# The output stays under ZZ9000OS/build/ so clean firmware rebuilds can remove
# it, and so the source tarball is not vendored into the repository.

set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
OS_DIR="$SCRIPT_DIR/ZZ9000_proto.sdk/ZZ9000OS"
VERSION="${LIBJPEG_TURBO_VERSION:-3.1.4.1}"
SHA256="${LIBJPEG_TURBO_SHA256:-ecae8008e2cc9ade2f2c1bb9d5e6d4fb73e7c433866a056bd82980741571a022}"
DEPS_DIR="$OS_DIR/build/deps/libjpeg-turbo"
VERSION_DIR="$DEPS_DIR/$VERSION"
SRC_DIR="$VERSION_DIR/src"
BUILD_DIR="$VERSION_DIR/build-simd"
ARCHIVE="$DEPS_DIR/libjpeg-turbo-$VERSION.tar.gz"
URL="https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$VERSION/libjpeg-turbo-$VERSION.tar.gz"

for tool in arm-none-eabi-gcc arm-none-eabi-ar arm-none-eabi-ranlib cmake make wget; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "ERROR: $tool not found on PATH." >&2
    exit 1
  fi
done

if [ -f "$BUILD_DIR/libjpeg.a" ]; then
  echo "[libjpeg-turbo] already built: $BUILD_DIR/libjpeg.a"
  exit 0
fi

mkdir -p "$DEPS_DIR" "$VERSION_DIR"

if [ ! -f "$ARCHIVE" ]; then
  echo "[libjpeg-turbo] downloading $URL"
  wget -q "$URL" -O "$ARCHIVE"
fi

if command -v sha256sum >/dev/null 2>&1; then
  ACTUAL_SHA256="$(sha256sum "$ARCHIVE" | awk '{print $1}')"
else
  ACTUAL_SHA256="$(shasum -a 256 "$ARCHIVE" | awk '{print $1}')"
fi
if [ "$ACTUAL_SHA256" != "$SHA256" ]; then
  echo "ERROR: libjpeg-turbo archive checksum mismatch." >&2
  echo "  expected: $SHA256" >&2
  echo "  actual:   $ACTUAL_SHA256" >&2
  rm -f "$ARCHIVE"
  exit 1
fi

if [ ! -f "$SRC_DIR/CMakeLists.txt" ]; then
  echo "[libjpeg-turbo] extracting source"
  rm -rf "$SRC_DIR"
  mkdir -p "$SRC_DIR"
  tar xzf "$ARCHIVE" --strip-components=1 -C "$SRC_DIR"
fi

mkdir -p "$BUILD_DIR"

echo "[libjpeg-turbo] configuring NEON static libjpeg"
cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
  -DCMAKE_SYSTEM_NAME=Generic \
  -DCMAKE_SYSTEM_PROCESSOR=arm \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_C_COMPILER=arm-none-eabi-gcc \
  -DCMAKE_ASM_COMPILER=arm-none-eabi-gcc \
  -DCMAKE_AR=arm-none-eabi-ar \
  -DCMAKE_RANLIB=arm-none-eabi-ranlib \
  -DCMAKE_C_FLAGS="-mcpu=cortex-a9 -marm -mfpu=neon -mfloat-abi=hard -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-strict-aliasing" \
  -DCMAKE_ASM_FLAGS="-mcpu=cortex-a9 -marm -mfpu=neon -mfloat-abi=hard" \
  -DENABLE_SHARED=FALSE \
  -DENABLE_STATIC=TRUE \
  -DWITH_TURBOJPEG=FALSE \
  -DWITH_JAVA=FALSE \
  -DWITH_SIMD=TRUE \
  -DREQUIRE_SIMD=TRUE \
  -DWITH_ARITH_DEC=FALSE \
  -DWITH_ARITH_ENC=FALSE \
  -DWITH_TOOLS=FALSE \
  -DWITH_TESTS=FALSE

echo "[libjpeg-turbo] building jpeg-static"
cmake --build "$BUILD_DIR" --target jpeg-static --parallel
echo "[libjpeg-turbo] done: $BUILD_DIR/libjpeg.a"
