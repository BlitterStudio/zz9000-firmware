#!/bin/sh
# Build and run the m68k actual-source sound DataType contract harness.
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

SDK_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUTPUT_DIR=${ZZ9K_SOUND_DATATYPE_TEST_OUTPUT:-"$SDK_ROOT/build/datatype-verification"}
IMAGE=${ZZ9K_M68K_IMAGE:-amigadev/crosstools:m68k-amigaos-gcc10}

mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR=$(CDPATH= cd -- "$OUTPUT_DIR" && pwd)
MSYS_NO_PATHCONV=1 docker run --rm \
  -v "$SDK_ROOT:/src/sdk:ro" \
  -v "$OUTPUT_DIR:/out" \
  -w /work "$IMAGE" sh -c '
set -eu
mkdir -p /work/source/sdk
tar -C /src/sdk --exclude=build -cf - . | tar -C /work/source/sdk -xf -
cd /work/source/sdk
m68k-amigaos-gcc -fcommon -noixemul -nostartfiles -Os \
  -fno-builtin -fno-stack-check -ffunction-sections -fdata-sections \
  tests/sound_datatype_contract_start.S \
  tests/sound_datatype_contract_test.c \
  -Wl,--gc-sections -o /out/sound_datatype_contract_test
'

# Use a relative program name: Vamos interprets drive-letter paths as Amiga volumes.
cd "$OUTPUT_DIR"
if command -v py >/dev/null 2>&1; then
  py -3 -m amitools.tools.vamos ./sound_datatype_contract_test
else
  python3 -m amitools.tools.vamos ./sound_datatype_contract_test
fi
