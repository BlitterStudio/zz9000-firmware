#!/bin/sh
# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

set -e

IMAGE="${IMAGE:-debian:bookworm-slim}"
OUTPUT="${OUTPUT:-bootimage_work/BOOT-sdk-docker.bin}"
CLEAN=1
BOOTIMAGE=1

while [ "$#" -gt 0 ]; do
  case "$1" in
    --no-clean)
      CLEAN=0
      ;;
    --no-bootimage)
      BOOTIMAGE=0
      ;;
    --output)
      shift
      OUTPUT="$1"
      ;;
    -h|--help)
      cat <<'EOF'
Usage: ./build_firmware_docker.sh [--no-clean] [--no-bootimage] [--output PATH]

Builds ZZ9000OS in Docker using the same official Arm GNU Toolchain and
bootgen flow as CI. Toolchain and bootgen are cached in Docker volumes.
EOF
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
  shift
done

REPO_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

docker run --rm \
  -v "$REPO_ROOT:/work" \
  -v zz9000-arm-toolchain:/opt/arm-gnu-toolchain \
  -v zz9000-bootgen:/opt/bootgen \
  -w /work \
  -e CLEAN="$CLEAN" \
  -e BOOTIMAGE="$BOOTIMAGE" \
  -e OUTPUT="$OUTPUT" \
  "$IMAGE" bash -lc '
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  wget xz-utils make cmake perl ca-certificates python3 g++ libssl-dev flex bison git p7zip-full

ARM_TOOLCHAIN_VERSION=13.2.rel1
TC_BASE="arm-gnu-toolchain-${ARM_TOOLCHAIN_VERSION}-x86_64-arm-none-eabi"
TC_DIR=/opt/arm-gnu-toolchain
if [ ! -x "$TC_DIR/bin/arm-none-eabi-gcc" ]; then
  rm -rf "$TC_DIR"/*
  wget -q \
    "https://developer.arm.com/-/media/Files/downloads/gnu/${ARM_TOOLCHAIN_VERSION}/binrel/${TC_BASE}.tar.xz" \
    -O /tmp/toolchain.tar.xz
  tar xf /tmp/toolchain.tar.xz --strip-components=1 -C "$TC_DIR"
  rm /tmp/toolchain.tar.xz
fi

if [ ! -x /opt/bootgen/bootgen ]; then
  rm -rf /tmp/bootgen-src /opt/bootgen/*
  git clone --depth 1 --branch master https://github.com/Xilinx/bootgen.git \
    /tmp/bootgen-src
  make -C /tmp/bootgen-src -j"$(nproc)"
  if [ -x /tmp/bootgen-src/bootgen ]; then
    cp /tmp/bootgen-src/bootgen /opt/bootgen/bootgen
  elif [ -x /tmp/bootgen-src/build/bin/bootgen ]; then
    cp /tmp/bootgen-src/build/bin/bootgen /opt/bootgen/bootgen
  else
    find /tmp/bootgen-src -maxdepth 4 -type f -name bootgen -print
    echo "bootgen executable not found after build" >&2
    exit 1
  fi
fi

export PATH="$TC_DIR/bin:$PATH"
export BOOTGEN=/opt/bootgen/bootgen

if [ "$CLEAN" = "1" ]; then
  ./build_firmware.sh clean
fi
./build_firmware.sh

if [ "$BOOTIMAGE" = "1" ]; then
  ./build_bootimage.sh --output "$OUTPUT"
fi
'
