#!/bin/bash
# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build zlib as a static ARM archive for firmware image codecs.
#
# The source and build output stay under ZZ9000OS/build/deps so clean
# firmware rebuilds can remove them without vendoring third-party code.

set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
OS_DIR="$SCRIPT_DIR/ZZ9000_proto.sdk/ZZ9000OS"
VERSION="${ZLIB_VERSION:-1.3.2}"
SHA256="${ZLIB_SHA256:-d7a0654783a4da529d1bb793b7ad9c3318020af77667bcae35f95d0e42a792f3}"
DEPS_DIR="$OS_DIR/build/deps/zlib"
VERSION_DIR="$DEPS_DIR/$VERSION"
SRC_DIR="$VERSION_DIR/src"
BUILD_DIR="$VERSION_DIR/build"
ARCHIVE="$DEPS_DIR/zlib-$VERSION.tar.xz"
URL="https://zlib.net/zlib-$VERSION.tar.xz"
# zlib.net serves only the current release at the root and moves older versions
# to fossils/, so fall back there. The sha256 check below guards correctness
# regardless of which mirror served the archive.
URL_FOSSILS="https://zlib.net/fossils/zlib-$VERSION.tar.xz"

for tool in arm-none-eabi-gcc arm-none-eabi-ar arm-none-eabi-ranlib cmake make wget; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "ERROR: $tool not found on PATH." >&2
    exit 1
  fi
done

if [ -f "$BUILD_DIR/libz.a" ]; then
  echo "[zlib] already built: $BUILD_DIR/libz.a"
  exit 0
fi

mkdir -p "$DEPS_DIR" "$VERSION_DIR"

if [ ! -f "$ARCHIVE" ]; then
  echo "[zlib] downloading $URL"
  wget -q "$URL" -O "$ARCHIVE" || {
    echo "[zlib] primary URL failed; trying $URL_FOSSILS"
    wget -q "$URL_FOSSILS" -O "$ARCHIVE"
  }
fi

if command -v sha256sum >/dev/null 2>&1; then
  ACTUAL_SHA256="$(sha256sum "$ARCHIVE" | awk '{print $1}')"
else
  ACTUAL_SHA256="$(shasum -a 256 "$ARCHIVE" | awk '{print $1}')"
fi
if [ "$ACTUAL_SHA256" != "$SHA256" ]; then
  echo "ERROR: zlib archive checksum mismatch." >&2
  echo "  expected: $SHA256" >&2
  echo "  actual:   $ACTUAL_SHA256" >&2
  rm -f "$ARCHIVE"
  exit 1
fi

if [ ! -f "$SRC_DIR/CMakeLists.txt" ]; then
  echo "[zlib] extracting source"
  rm -rf "$SRC_DIR"
  mkdir -p "$SRC_DIR"
  tar xJf "$ARCHIVE" --strip-components=1 -C "$SRC_DIR"
fi

mkdir -p "$BUILD_DIR"

echo "[zlib] configuring static archive"
cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
  -DCMAKE_SYSTEM_NAME=Generic \
  -DCMAKE_SYSTEM_PROCESSOR=arm \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_C_COMPILER=arm-none-eabi-gcc \
  -DCMAKE_AR=arm-none-eabi-ar \
  -DCMAKE_RANLIB=arm-none-eabi-ranlib \
  -DCMAKE_C_FLAGS="-mcpu=cortex-a9 -marm -mfpu=neon -mfloat-abi=hard -O2 -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-strict-aliasing" \
  -DZLIB_BUILD_SHARED=OFF \
  -DZLIB_BUILD_STATIC=ON \
  -DZLIB_BUILD_TESTING=OFF \
  -DZLIB_INSTALL=OFF

echo "[zlib] building zlibstatic"
cmake --build "$BUILD_DIR" --target zlibstatic --parallel
echo "[zlib] done: $BUILD_DIR/libz.a"
