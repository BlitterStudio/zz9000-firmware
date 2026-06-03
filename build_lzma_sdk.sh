#!/bin/bash
# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Fetch the public-domain LZMA SDK decoder sources used by SDK codec services.
#
# The source stays under ZZ9000OS/build/deps so firmware clean builds can
# remove it without vendoring third-party code.

set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
OS_DIR="$SCRIPT_DIR/ZZ9000_proto.sdk/ZZ9000OS"
VERSION="${LZMA_SDK_VERSION:-26.01}"
VERSION_FILE="$(printf '%s' "$VERSION" | tr -d '.')"
SHA256="${LZMA_SDK_SHA256:-b860f17f9df3c0524dd2ef2c639ab5e43ad0006b77b8f7bb6d191bf528536885}"
DEPS_DIR="$OS_DIR/build/deps/lzma-sdk"
VERSION_DIR="$DEPS_DIR/$VERSION"
SRC_DIR="$VERSION_DIR/src"
ARCHIVE="$DEPS_DIR/lzma-$VERSION.7z"
URL="${LZMA_SDK_URL:-https://www.7-zip.org/a/lzma${VERSION_FILE}.7z}"

if command -v 7z >/dev/null 2>&1; then
  SEVENZIP=7z
elif command -v 7za >/dev/null 2>&1; then
  SEVENZIP=7za
elif command -v 7zr >/dev/null 2>&1; then
  SEVENZIP=7zr
else
  echo "ERROR: 7z/7za/7zr not found on PATH." >&2
  exit 1
fi

for tool in wget "$SEVENZIP"; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "ERROR: $tool not found on PATH." >&2
    exit 1
  fi
done

if [ -f "$SRC_DIR/C/LzmaDec.c" ] &&
   [ -f "$SRC_DIR/C/LzmaDec.h" ] &&
   [ -f "$SRC_DIR/C/7zTypes.h" ]; then
  echo "[lzma-sdk] already extracted: $SRC_DIR"
  exit 0
fi

mkdir -p "$DEPS_DIR" "$VERSION_DIR"

if [ ! -f "$ARCHIVE" ]; then
  echo "[lzma-sdk] downloading $URL"
  wget -q "$URL" -O "$ARCHIVE"
fi

if command -v sha256sum >/dev/null 2>&1; then
  ACTUAL_SHA256="$(sha256sum "$ARCHIVE" | awk '{print $1}')"
else
  ACTUAL_SHA256="$(shasum -a 256 "$ARCHIVE" | awk '{print $1}')"
fi
if [ "$ACTUAL_SHA256" != "$SHA256" ]; then
  echo "ERROR: LZMA SDK archive checksum mismatch." >&2
  echo "  expected: $SHA256" >&2
  echo "  actual:   $ACTUAL_SHA256" >&2
  rm -f "$ARCHIVE"
  exit 1
fi

echo "[lzma-sdk] extracting source"
rm -rf "$SRC_DIR"
mkdir -p "$SRC_DIR"
"$SEVENZIP" x -y -o"$SRC_DIR" "$ARCHIVE" >/dev/null

if [ ! -f "$SRC_DIR/C/LzmaDec.c" ] ||
   [ ! -f "$SRC_DIR/C/LzmaDec.h" ] ||
   [ ! -f "$SRC_DIR/C/7zTypes.h" ]; then
  echo "ERROR: LZMA SDK extraction did not produce decoder sources." >&2
  exit 1
fi

echo "[lzma-sdk] done: $SRC_DIR"
