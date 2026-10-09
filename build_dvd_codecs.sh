#!/bin/bash
# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build decoder-only libmpeg2 and liba52 static archives for ZZ9000OS.
# The pinned source archives and all derived files stay below build/deps; no
# upstream source is vendored into the repository. The GPL-2.0-or-later
# license notices covering both libraries are checked in under
# ZZ9000_proto.sdk/ZZ9000OS/src/dvd_codecs/.
#
# Licensing (verified against the pinned archives themselves): every decoder
# translation unit in both libraries carries the grant "either version 2 of
# the License, or (at your option) any later version", i.e. GPL-2.0-or-later.
# The firmware is GPL-3.0-or-later, so the combined work is distributed under
# GPLv3 via the "or any later version" clause. No GPL-2-only component is
# imported.
#
# Decoder-only subsets (the mpeg2dec/a52dec front ends, libvo/libao and the
# color converters stay out): the archives never contain main().

set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
OS_DIR="$SCRIPT_DIR/ZZ9000_proto.sdk/ZZ9000OS"

LIBMPEG2_VERSION="${LIBMPEG2_VERSION:-0.5.1}"
# libmpeg2.sourceforge.io/files/ 404s at the time of pinning; the VideoLAN
# contrib mirror serves the byte-identical upstream tarball
# (524776 bytes; hash cross-checked against Debian mpeg2dec_0.5.1.orig).
LIBMPEG2_SHA256="${LIBMPEG2_SHA256:-dee22e893cb5fc2b2b6ebd60b88478ab8556cb3b93f9a0d7ce8f3b61851871d4}"
LIBMPEG2_URL="${LIBMPEG2_URL:-https://download.videolan.org/contrib/libmpeg2/libmpeg2-${LIBMPEG2_VERSION}.tar.gz}"

A52_VERSION="${A52_VERSION:-0.7.4}"
# liba52.sourceforge.io/files/ 404s at the time of pinning; the BLFS/OSUOSL
# mirror serves the byte-identical upstream tarball (241507 bytes; hash
# cross-checked against Debian a52dec_0.7.4.orig).
A52_SHA256="${A52_SHA256:-a21d724ab3b3933330194353687df82c475b5dfb997513eef4c25de6c865ec33}"
A52_URL="${A52_URL:-http://ftp.osuosl.org/pub/blfs/conglomeration/a52dec/a52dec-${A52_VERSION}.tar.gz}"

TARGET="${LIBDVD_TARGET:-arm}"

usage() {
    cat <<'EOF'
Usage: build_dvd_codecs.sh [--host|--arm]

Build decoder-only libmpeg2 and liba52 static archives for the U11 DVD
experiment. The default target is ARM firmware; --host builds native archives
for host correctness tests. Both builds use the portable C decoder cores with
a hand-written config.h (ATTRIBUTE_ALIGNED_MAX=16, builtin_expect) so no
autotools probe runs inside the firmware build.
EOF
}

case "$#" in
    0) ;;
    1)
        case "$1" in
            --host) TARGET=host ;;
            --arm) TARGET=arm ;;
            --help|-h) usage; exit 0 ;;
            *) usage >&2; exit 2 ;;
        esac ;;
    *) usage >&2; exit 2 ;;
esac
case "$TARGET" in
    arm|host) ;;
    *) echo "ERROR: LIBDVD_TARGET must be arm or host." >&2; exit 2 ;;
esac

fetch() {
    local url="$1" archive="$2" sha="$3" name="$4" actual
    if [ ! -f "$archive" ]; then
        echo "[$name] downloading $url"
        if command -v curl >/dev/null 2>&1; then
            curl --fail --location --silent --show-error "$url" -o "$archive"
        elif command -v wget >/dev/null 2>&1; then
            wget -q "$url" -O "$archive"
        else
            echo "ERROR: install curl or wget to download $name." >&2
            exit 1
        fi
    fi
    if command -v sha256sum >/dev/null 2>&1; then
        actual="$(sha256sum "$archive" | awk '{print $1}')"
    else
        actual="$(shasum -a 256 "$archive" | awk '{print $1}')"
    fi
    if [ "$actual" != "$sha" ]; then
        echo "ERROR: $name archive checksum mismatch." >&2
        echo "  expected: $sha" >&2
        echo "  actual:   $actual" >&2
        rm -f "$archive"
        exit 1
    fi
}

if [ "$TARGET" = arm ]; then
    : "${ARM_NONE_EABI_CC:=arm-none-eabi-gcc}"
    : "${ARM_NONE_EABI_AR:=arm-none-eabi-ar}"
    : "${ARM_NONE_EABI_RANLIB:=arm-none-eabi-ranlib}"
    for tool in "$ARM_NONE_EABI_CC" "$ARM_NONE_EABI_AR" "$ARM_NONE_EABI_RANLIB"; do
        command -v "$tool" >/dev/null 2>&1 || {
            echo "ERROR: required ARM tool not found: $tool" >&2; exit 1; }
    done
    CC_CMD="$(command -v "$ARM_NONE_EABI_CC")"
    AR_CMD="$(command -v "$ARM_NONE_EABI_AR")"
    RANLIB_CMD="$(command -v "$ARM_NONE_EABI_RANLIB")"
    CFLAGS_TARGET="-mcpu=cortex-a9 -marm -mfpu=neon -mfloat-abi=hard \
-O3 -ffunction-sections -fdata-sections -fno-unwind-tables \
-fno-asynchronous-unwind-tables -fno-strict-aliasing"
else
    : "${CC_FOR_BUILD:=cc}"
    : "${AR_FOR_BUILD:=ar}"
    : "${RANLIB_FOR_BUILD:=ranlib}"
    for tool in "$CC_FOR_BUILD" "$AR_FOR_BUILD" "$RANLIB_FOR_BUILD"; do
        command -v "$tool" >/dev/null 2>&1 || {
            echo "ERROR: required host tool not found: $tool" >&2; exit 1; }
    done
    CC_CMD="$(command -v "$CC_FOR_BUILD")"
    AR_CMD="$(command -v "$AR_FOR_BUILD")"
    RANLIB_CMD="$(command -v "$RANLIB_FOR_BUILD")"
    CFLAGS_TARGET="-O2 -ffunction-sections -fdata-sections -fno-strict-aliasing"
fi

build_dir_for() {
    local lib="$1" ver="$2"
    echo "$OS_DIR/build/deps/$lib/$ver"
}

# --- libmpeg2 (decoder core only) ----------------------------------------
MPEG2_DIR="$(build_dir_for libmpeg2 "$LIBMPEG2_VERSION")"
MPEG2_ARCHIVE="$MPEG2_DIR/libmpeg2-$LIBMPEG2_VERSION.tar.gz"
MPEG2_SRC="$MPEG2_DIR/src"
MPEG2_OUT="$MPEG2_DIR/$TARGET"
MPEG2_LIB="$MPEG2_OUT/libmpeg2.a"
mkdir -p "$MPEG2_DIR"
fetch "$LIBMPEG2_URL" "$MPEG2_ARCHIVE" "$LIBMPEG2_SHA256" libmpeg2
if [ ! -f "$MPEG2_SRC/libmpeg2/decode.c" ]; then
    rm -rf "$MPEG2_SRC"
    mkdir -p "$MPEG2_SRC"
    tar xzf "$MPEG2_ARCHIVE" --strip-components=1 -C "$MPEG2_SRC"
fi
if [ ! -f "$MPEG2_LIB" ]; then
    rm -rf "$MPEG2_OUT"
    mkdir -p "$MPEG2_OUT/include" "$MPEG2_OUT/obj"
    # Portable decoder core; no ARCH_* define so every dispatcher selects
    # the _c implementation (the ARMv4 assembly paths predate NEON and the
    # MMX/Altivec paths are irrelevant to cortex-a9).
    cat > "$MPEG2_OUT/include/config.h" <<'EOF'
/* Generated by build_dvd_codecs.sh - portable C decoder configuration. */
#define ATTRIBUTE_ALIGNED_MAX 16
#define HAVE_BUILTIN_EXPECT 1
EOF
    M2_TUS="alloc cpu_accel cpu_state decode header idct motion_comp slice"
    for tu in $M2_TUS; do
        "$CC_CMD" $CFLAGS_TARGET -I"$MPEG2_SRC/include" \
            -I"$MPEG2_OUT/include" -c "$MPEG2_SRC/libmpeg2/$tu.c" \
            -o "$MPEG2_OUT/obj/$tu.o"
    done
    "$AR_CMD" rcs "$MPEG2_LIB" "$MPEG2_OUT"/obj/*.o
    "$RANLIB_CMD" "$MPEG2_LIB"
    cp "$MPEG2_SRC/include/mpeg2.h" "$MPEG2_OUT/include/mpeg2.h"
    rm -rf "$MPEG2_OUT/obj"
    echo "[libmpeg2] built ($TARGET): $MPEG2_LIB"
fi

# --- liba52 (decoder core only) ------------------------------------------
A52_DIR="$(build_dir_for a52dec "$A52_VERSION")"
A52_ARCHIVE="$A52_DIR/a52dec-$A52_VERSION.tar.gz"
A52_SRC="$A52_DIR/src"
A52_OUT="$A52_DIR/$TARGET"
A52_LIB="$A52_OUT/liba52.a"
mkdir -p "$A52_DIR"
fetch "$A52_URL" "$A52_ARCHIVE" "$A52_SHA256" a52dec
if [ ! -f "$A52_SRC/liba52/parse.c" ]; then
    rm -rf "$A52_SRC"
    mkdir -p "$A52_SRC"
    tar xzf "$A52_ARCHIVE" --strip-components=1 -C "$A52_SRC"
fi
if [ ! -f "$A52_LIB" ]; then
    rm -rf "$A52_OUT"
    mkdir -p "$A52_OUT/include" "$A52_OUT/obj"
    # Default precision (sample_t = float); no runtime accel detection.
    cat > "$A52_OUT/include/config.h" <<'EOF'
/* Generated by build_dvd_codecs.sh - portable C decoder configuration. */
#define ATTRIBUTE_ALIGNED_MAX 16
#define HAVE_BUILTIN_EXPECT 1
EOF
    A52_TUS="bitstream bit_allocate downmix imdct parse"
    for tu in $A52_TUS; do
        "$CC_CMD" $CFLAGS_TARGET -I"$A52_SRC/include" -I"$A52_SRC/liba52" \
            -I"$A52_OUT/include" -c "$A52_SRC/liba52/$tu.c" \
            -o "$A52_OUT/obj/$tu.o"
    done
    "$AR_CMD" rcs "$A52_LIB" "$A52_OUT"/obj/*.o
    "$RANLIB_CMD" "$A52_LIB"
    cp "$A52_SRC/include/a52.h" "$A52_OUT/include/a52.h"
    cp "$A52_SRC/include/mm_accel.h" "$A52_OUT/include/mm_accel.h"
    cp "$A52_SRC/include/attributes.h" "$A52_OUT/include/attributes.h"
    rm -rf "$A52_OUT/obj"
    echo "[liba52] built ($TARGET): $A52_LIB"
fi
