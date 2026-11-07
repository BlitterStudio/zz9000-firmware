#!/bin/sh
# Build the PIE-safe app libc/libm sysroot from newlib 4.3.0.20230120, the
# release used by Arm GNU Toolchain 13.2.rel1. Stock archives are non-PIC.
# Suggested Docker cache volumes:
#   -v zzapp-pic-newlib:/opt/zzapp-pic-newlib
#   -v zzapp-newlib-cache:/opt/zzapp-newlib-cache
# Set ZZAPP_PIC_SYSROOT=/opt/zzapp-pic-newlib before running.
# SPDX-License-Identifier: MIT
set -e
set -u

NEWLIB_VERSION=4.3.0.20230120
NEWLIB_SHA256=83a62a99af59e38eb9b0c58ed092ee24d700fff43a22c03e433955113ef35150
NEWLIB_URL=https://sourceware.org/pub/newlib/newlib-${NEWLIB_VERSION}.tar.gz
TARGET=arm-none-eabi
PREFIX=${ZZAPP_PIC_SYSROOT:?set ZZAPP_PIC_SYSROOT to the PIC app sysroot}
CACHE=${ZZAPP_NEWLIB_CACHE:-"$(dirname "$PREFIX")/zzapp-newlib-cache"}
BUILD_ROOT=${ZZAPP_NEWLIB_BUILD:-"$(pwd)/.build-pic-newlib"}
JOBS=${JOBS:-2}

: "${CC:=gcc}"
: "${CROSS_COMPILE:=arm-none-eabi-}"
command -v "${CROSS_COMPILE}gcc" >/dev/null
command -v "$CC" >/dev/null
command -v make >/dev/null
command -v tar >/dev/null
command -v sha256sum >/dev/null

mkdir -p "$CACHE"
ARCHIVE="$CACHE/newlib-${NEWLIB_VERSION}.tar.gz"
if [ ! -f "$ARCHIVE" ]; then
    if command -v curl >/dev/null; then
        curl -fsSL "$NEWLIB_URL" -o "$ARCHIVE"
    else
        wget -q "$NEWLIB_URL" -O "$ARCHIVE"
    fi
fi
echo "$NEWLIB_SHA256  $ARCHIVE" | sha256sum -c -

rm -rf "$BUILD_ROOT"
mkdir -p "$BUILD_ROOT/src" "$BUILD_ROOT/build"
tar -xzf "$ARCHIVE" -C "$BUILD_ROOT/src" --strip-components=1
cd "$BUILD_ROOT/build"

TARGET_CFLAGS='-mcpu=cortex-a9 -marm -mfpu=neon -mfloat-abi=hard -fPIC -O2 -fno-strict-aliasing -ffunction-sections -fdata-sections'
"$BUILD_ROOT/src/configure" \
    --target="$TARGET" \
    --prefix="$PREFIX" \
    --disable-shared \
    --disable-multilib \
    --disable-nls \
    --disable-newlib-supplied-syscalls \
    --disable-newlib-multithread \
    --enable-newlib-io-long-long \
    --enable-newlib-io-c99-formats \
    --enable-newlib-retargetable-locking
make -j"$JOBS" \
    CC_FOR_TARGET="${CROSS_COMPILE}gcc" \
    AR_FOR_TARGET="${CROSS_COMPILE}ar" \
    RANLIB_FOR_TARGET="${CROSS_COMPILE}ranlib" \
    CFLAGS_FOR_TARGET="$TARGET_CFLAGS" \
    CXXFLAGS_FOR_TARGET="$TARGET_CFLAGS"
make install

LIBDIR="$PREFIX/$TARGET/lib"
if [ ! -f "$LIBDIR/libc.a" ] || [ ! -f "$LIBDIR/libm.a" ]; then
    echo "PIC newlib install incomplete: expected libc.a and libm.a in $LIBDIR" >&2
    exit 1
fi
printf '%s\n' "$TARGET_CFLAGS" > "$PREFIX/.zzapp-pic-newlib-cflags"
printf '%s\n' "$NEWLIB_VERSION" > "$PREFIX/.zzapp-pic-newlib-version"
