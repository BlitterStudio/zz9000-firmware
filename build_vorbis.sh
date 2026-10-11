#!/bin/bash
# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build a decoder-only libogg + fixed-point Tremor static archive for ZZ9000OS
# (U8 Ogg Vorbis). The pinned upstream archives and every derived file stay
# below build/deps; no upstream source is vendored into the repository. The
# upstream BSD-3-Clause notices (COPYING.libogg, COPYING.tremor) are checked in
# under ZZ9000_proto.sdk/ZZ9000OS/src/vorbis/.
#
# Only decode translation units are compiled: libogg framing/bitwise (the
# firmware links just the sync/page and bit-reader entry points; the
# page/packet writers are discarded by --gc-sections) and Tremor's synthesis
# core. Not compiled: Tremor's vorbisfile/seeking layer and examples, the
# ARM assembly (_ARM_ASSEM_ stays undefined), the _LOW_ACCURACY_ path, and
# anything float. There is no Vorbis encoder in Tremor. Every
# malloc/calloc/realloc/free inside libogg and Tremor is routed through
# src/sdk_vorbis_alloc.h into the stream's tracked arena.

set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
OS_DIR="$SCRIPT_DIR/ZZ9000_proto.sdk/ZZ9000OS"
OGG_VERSION="${LIBOGG_VERSION:-1.3.6}"
# Release 1.3.6 (2025-06-16); hash of the downloads.xiph.org tarball, which
# is byte-identical to the GitHub xiph/ogg v1.3.6 release asset.
OGG_SHA256="${LIBOGG_SHA256:-5c8253428e181840cd20d41f3ca16557a9cc04bad4a3d04cce84808677fa1061}"
OGG_URL="${LIBOGG_URL:-https://downloads.xiph.org/releases/ogg/libogg-${OGG_VERSION}.tar.xz}"
# Tremor has no release tarballs; pin the gitlab.xiph.org master commit of
# 2025-04-03 (includes the CVE-2018-5147 codebook fix and the 2024 UB fixes).
TREMOR_COMMIT="${TREMOR_COMMIT:-820fb3237ea81af44c9cc468c8b4e20128e3e5ad}"
TREMOR_SHA256="${TREMOR_SHA256:-3974455fe776a41615f60e05c85733c1bb78bff310794cbb0236f8af4b44f583}"
TREMOR_URL="${TREMOR_URL:-https://gitlab.xiph.org/xiph/tremor/-/archive/${TREMOR_COMMIT}/tremor-${TREMOR_COMMIT}.tar.gz}"
TARGET="${LIBVORBIS_TARGET:-arm}"

usage() {
    cat <<'EOF'
Usage: build_vorbis.sh [--host|--arm]

Build the pinned decoder-only libogg + Tremor archive (libvorbisdec.a). The
default target is ARM firmware; --host builds a native archive for the host
tests.
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
    *) echo "ERROR: LIBVORBIS_TARGET must be arm or host." >&2; exit 2 ;;
esac

ALLOC_HEADER="$OS_DIR/src/sdk_vorbis_alloc.h"
if [ ! -f "$ALLOC_HEADER" ]; then
    echo "ERROR: missing $ALLOC_HEADER" >&2
    exit 1
fi

DEPS_DIR="$OS_DIR/build/deps/vorbis/ogg-${OGG_VERSION}-tremor-${TREMOR_COMMIT:0:12}"
OGG_ARCHIVE="$DEPS_DIR/libogg-$OGG_VERSION.tar.xz"
TREMOR_ARCHIVE="$DEPS_DIR/tremor-$TREMOR_COMMIT.tar.gz"
OGG_SRC="$DEPS_DIR/src/libogg"
TREMOR_SRC="$DEPS_DIR/src/tremor"
OUT_DIR="$DEPS_DIR/$TARGET"
LIB="$OUT_DIR/libvorbisdec.a"

fetch() {
    local url="$1" dest="$2" sha="$3" actual
    if [ ! -f "$dest" ]; then
        echo "[vorbis] downloading $url"
        if command -v curl >/dev/null 2>&1; then
            curl --fail --location --silent --show-error "$url" -o "$dest"
        elif command -v wget >/dev/null 2>&1; then
            wget -q "$url" -O "$dest"
        else
            echo "ERROR: install curl or wget to download $url." >&2
            exit 1
        fi
    fi
    if command -v sha256sum >/dev/null 2>&1; then
        actual="$(sha256sum "$dest" | awk '{print $1}')"
    else
        actual="$(shasum -a 256 "$dest" | awk '{print $1}')"
    fi
    if [ "$actual" != "$sha" ]; then
        echo "ERROR: checksum mismatch for $dest." >&2
        echo "  expected: $sha" >&2
        echo "  actual:   $actual" >&2
        rm -f "$dest"
        exit 1
    fi
}

mkdir -p "$DEPS_DIR"
fetch "$OGG_URL" "$OGG_ARCHIVE" "$OGG_SHA256"
fetch "$TREMOR_URL" "$TREMOR_ARCHIVE" "$TREMOR_SHA256"
if [ ! -f "$OGG_SRC/src/framing.c" ]; then
    rm -rf "$OGG_SRC"
    mkdir -p "$OGG_SRC"
    tar xJf "$OGG_ARCHIVE" --strip-components=1 -C "$OGG_SRC"
fi
if [ ! -f "$TREMOR_SRC/synthesis.c" ]; then
    rm -rf "$TREMOR_SRC"
    mkdir -p "$TREMOR_SRC"
    tar xzf "$TREMOR_ARCHIVE" --strip-components=1 -C "$TREMOR_SRC"
fi

if [ "$TARGET" = arm ]; then
    : "${ARM_NONE_EABI_CC:=arm-none-eabi-gcc}"
    : "${ARM_NONE_EABI_AR:=arm-none-eabi-ar}"
    : "${ARM_NONE_EABI_RANLIB:=arm-none-eabi-ranlib}"
    CC_TOOL="$ARM_NONE_EABI_CC"; AR_TOOL="$ARM_NONE_EABI_AR"; RANLIB_TOOL="$ARM_NONE_EABI_RANLIB"
    CFLAGS_TARGET="-mcpu=cortex-a9 -marm -mfpu=neon -mfloat-abi=hard \
-O2 -ffunction-sections -fdata-sections -fno-unwind-tables \
-fno-asynchronous-unwind-tables -fno-strict-aliasing"
else
    : "${CC_FOR_BUILD:=cc}"
    : "${AR_FOR_BUILD:=ar}"
    : "${RANLIB_FOR_BUILD:=ranlib}"
    CC_TOOL="$CC_FOR_BUILD"; AR_TOOL="$AR_FOR_BUILD"; RANLIB_TOOL="$RANLIB_FOR_BUILD"
    CFLAGS_TARGET="-O2 -ffunction-sections -fdata-sections -fno-strict-aliasing"
fi
for tool in "$CC_TOOL" "$AR_TOOL" "$RANLIB_TOOL"; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "ERROR: required tool not found: $tool" >&2; exit 1; }
done

# Rebuild whenever the allocator shim or this recipe changed.
STAMP_INPUT="$(cat "$ALLOC_HEADER" "$0" | cksum | awk '{print $1}')"
if [ -f "$LIB" ] && [ -f "$OUT_DIR/.stamp" ] &&
   [ "$(cat "$OUT_DIR/.stamp")" = "$STAMP_INPUT" ]; then
    touch "$LIB"
    echo "[vorbis] up to date ($TARGET): $LIB"
    exit 0
fi

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/obj" "$OUT_DIR/include/ogg" "$OUT_DIR/include/tremor"
# libogg's configure-generated type header, written by hand (identical on
# the host and the Cortex-A9).
cat > "$OUT_DIR/include/ogg/config_types.h" <<'EOF'
/* Generated by build_vorbis.sh - libogg configuration types, as libogg's
 * configure picks them on both targets (32-bit int, 64-bit long long).
 * Not <stdint.h>: newlib's int32_t is long, which Tremor mixes with int. */
#ifndef __CONFIG_TYPES_H__
#define __CONFIG_TYPES_H__
typedef short ogg_int16_t;
typedef unsigned short ogg_uint16_t;
typedef int ogg_int32_t;
typedef unsigned int ogg_uint32_t;
typedef long long ogg_int64_t;
typedef unsigned long long ogg_uint64_t;
#endif
EOF
cp "$OGG_SRC/include/ogg/ogg.h" "$OGG_SRC/include/ogg/os_types.h" \
   "$OUT_DIR/include/ogg/"
# Public synthesis API plus the setup structures the backend inspects to
# bound codebook sizes before vorbis_synthesis_init().
cp "$TREMOR_SRC/ivorbiscodec.h" "$TREMOR_SRC/codec_internal.h" \
   "$TREMOR_SRC/codebook.h" "$OUT_DIR/include/tremor/"

COMMON_FLAGS="$CFLAGS_TARGET -DNDEBUG -DHAVE_ALLOCA_H \
-DSDK_VORBIS_REPLACE_ALLOCATORS -include $ALLOC_HEADER -I$OUT_DIR/include"
for tu in framing bitwise; do
    "$CC_TOOL" $COMMON_FLAGS -c "$OGG_SRC/src/$tu.c" -o "$OUT_DIR/obj/ogg_$tu.o"
done
TREMOR_TUS="block codebook floor0 floor1 info mapping0 mdct registry res012 \
sharedbook synthesis window"
for tu in $TREMOR_TUS; do
    "$CC_TOOL" $COMMON_FLAGS -I"$TREMOR_SRC" \
        -c "$TREMOR_SRC/$tu.c" -o "$OUT_DIR/obj/tremor_$tu.o"
done
"$AR_TOOL" rcs "$LIB" "$OUT_DIR"/obj/*.o
"$RANLIB_TOOL" "$LIB"
rm -rf "$OUT_DIR/obj"
printf '%s\n' "$STAMP_INPUT" > "$OUT_DIR/.stamp"
echo "[vorbis] built ($TARGET): $LIB"
