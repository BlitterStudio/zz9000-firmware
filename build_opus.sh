#!/bin/bash
# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Decoder-oriented libopus for ZZ9000OS WebM media sessions. Fixed-point,
# no float API, no runtime CPU detection, no extra programs. The neural
# PLC / DRED / OSCE paths added in 1.5+ are disabled: the card decodes
# the RFC 6716 stream and the archive stays smaller. Opus itself is
# single-threaded; there is no pthread build to turn off. The pinned
# upstream archive and every derived file stay below build/deps; the
# upstream BSD-3-Clause notice (COPYING.libopus) is checked in under
# ZZ9000_proto.sdk/ZZ9000OS/src/webm_codecs/.

set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
OS_DIR="$SCRIPT_DIR/ZZ9000_proto.sdk/ZZ9000OS"
VERSION="${LIBOPUS_VERSION:-1.6.1}"
SHA256="${LIBOPUS_SHA256:-6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1}"
URL="${LIBOPUS_URL:-https://downloads.xiph.org/releases/opus/opus-${VERSION}.tar.gz}"
TARGET="${LIBOPUS_TARGET:-arm}"
DEPS_DIR="$OS_DIR/build/deps/opus/${VERSION}"
SRC="$DEPS_DIR/src"
OUT="$DEPS_DIR/$TARGET"
LIB="$OUT/libopusdec.a"

case "${1:-}" in
    ""|--arm) TARGET=arm ;;
    --host) TARGET=host ;;
    -h|--help) echo "Usage: build_opus.sh [--arm|--host]"; exit 0 ;;
    *) echo "Usage: build_opus.sh [--arm|--host]" >&2; exit 2 ;;
esac
OUT="$DEPS_DIR/$TARGET"
LIB="$OUT/libopusdec.a"

mkdir -p "$DEPS_DIR"
if [ ! -f "$DEPS_DIR/opus-${VERSION}.tar.gz" ]; then
    echo "[opus] downloading $URL"
    if command -v curl >/dev/null 2>&1; then
        curl --fail --location --silent --show-error "$URL" -o "$DEPS_DIR/opus-${VERSION}.tar.gz"
    else
        wget -q "$URL" -O "$DEPS_DIR/opus-${VERSION}.tar.gz"
    fi
fi
if command -v sha256sum >/dev/null 2>&1; then
    actual="$(sha256sum "$DEPS_DIR/opus-${VERSION}.tar.gz" | awk '{print $1}')"
else
    actual="$(shasum -a 256 "$DEPS_DIR/opus-${VERSION}.tar.gz" | awk '{print $1}')"
fi
if [ "$actual" != "$SHA256" ]; then
    echo "ERROR: opus checksum mismatch" >&2
    echo "  expected: $SHA256" >&2
    echo "  actual:   $actual" >&2
    exit 1
fi
if [ ! -f "$SRC/configure" ]; then
    rm -rf "$SRC"
    mkdir -p "$SRC"
    tar xzf "$DEPS_DIR/opus-${VERSION}.tar.gz" --strip-components=1 -C "$SRC"
fi
if [ -f "$LIB" ] && [ "$SRC/configure" -ot "$LIB" ]; then
    echo "[opus] up to date: $LIB"
    exit 0
fi

BUILD="$DEPS_DIR/build-$TARGET"
rm -rf "$BUILD"
mkdir -p "$BUILD" "$OUT/include"
cd "$BUILD"
CFLAGS_COMMON="-O2 -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables"
CONF=(
    --prefix="$OUT"
    --disable-shared --enable-static
    --disable-extra-programs --disable-doc
    --disable-rtcd
    --enable-fixed-point --disable-float-api
    --disable-deep-plc --disable-dred --disable-osce
)
if [ "$TARGET" = arm ]; then
    : "${ARM_NONE_EABI_CC:=arm-none-eabi-gcc}"
    : "${ARM_NONE_EABI_AR:=arm-none-eabi-ar}"
    : "${ARM_NONE_EABI_RANLIB:=arm-none-eabi-ranlib}"
    export CC="$ARM_NONE_EABI_CC"
    export AR="$ARM_NONE_EABI_AR"
    export RANLIB="$ARM_NONE_EABI_RANLIB"
    # nosys so configure's link tests do not need a hosted exit().
    export CFLAGS="-mcpu=cortex-a9 -marm -mfpu=neon -mfloat-abi=hard $CFLAGS_COMMON -DSDK_VORBIS_REPLACE_ALLOCATORS -include $OS_DIR/src/sdk_vorbis_alloc.h"
    export LDFLAGS="-specs=nosys.specs"
    bash "$SRC/configure" --host=arm-none-eabi "${CONF[@]}"
else
    export CFLAGS="$CFLAGS_COMMON"
    bash "$SRC/configure" "${CONF[@]}"
fi
make -j"$(nproc 2>/dev/null || echo 4)"
make install
if [ -f "$OUT/lib/libopus.a" ]; then
    cp -a "$OUT/lib/libopus.a" "$LIB"
else
    cp -a .libs/libopus.a "$LIB"
fi
echo "[opus] built $LIB"
if [ "$TARGET" = arm ]; then
    arm-none-eabi-size -t "$LIB" | tail -1
fi
