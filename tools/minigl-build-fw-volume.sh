#!/bin/sh
# Build ZZ9000OS inside a Docker volume instead of a Windows bind mount:
# libpng's CMake static-lib step creates relative symlinks, which DrvFs
# bind mounts reject ("Operation not permitted"). Everything else matches
# build_firmware_docker.sh (same toolchain/bootgen volumes and flow).
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -e

IMAGE="${IMAGE:-debian:bookworm-slim}"
VOL=zz9000-fw-src
OUT_VOL=zz9000-fw-out

REPO_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

docker volume create "$VOL" >/dev/null
docker volume create "$OUT_VOL" >/dev/null

# MSYS rewrites container paths like /src otherwise.
export MSYS_NO_PATHCONV=1
if command -v cygpath >/dev/null 2>&1; then
  REPO_ROOT="$(cygpath -w "$REPO_ROOT")"
fi
# Fresh source tree in the volume from git archive (tracked files only).
docker run --rm -v "$VOL:/src" -v "$OUT_VOL:/out" "$IMAGE" bash -c 'rm -rf /src/*'
git -C "$REPO_ROOT" archive HEAD | docker run --rm -i -v "$VOL:/src" "$IMAGE" \
  tar x -C /src

docker run --rm \
  -v "$VOL:/src" \
  -v "$OUT_VOL:/out" \
  -v zz9000-arm-toolchain:/opt/arm-gnu-toolchain \
  -v zz9000-bootgen:/opt/bootgen \
  -w /src \
  -e OUTPUT=/out/BOOT-sdk-docker.bin \
  "$IMAGE" bash -lc '
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  wget xz-utils make cmake perl ca-certificates python3 g++ libssl-dev flex bison git p7zip-full unzip

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
  git clone --depth 1 --branch master https://github.com/Xilinx/bootgen.git /tmp/bootgen-src
  make -C /tmp/bootgen-src -j"$(nproc)"
  cp "$(find /tmp/bootgen-src -maxdepth 4 -type f -name bootgen | head -1)" /opt/bootgen/bootgen
fi
export PATH="$TC_DIR/bin:$PATH"
export BOOTGEN=/opt/bootgen/bootgen
./build_firmware.sh clean
./build_firmware.sh
./build_bootimage.sh --output "$OUTPUT"
echo "VOLUME_BUILD_OK"
'
