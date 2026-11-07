# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

param(
  [string]$Image = "debian:bookworm-slim",
  [string]$Output = "bootimage_work/BOOT-sdk-docker.bin",
  [string]$ExtraCFlags = "",
  [switch]$NoClean,
  [switch]$NoBootimage
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path $PSScriptRoot).Path
$CleanValue = if ($NoClean) { "0" } else { "1" }
$BootimageValue = if ($NoBootimage) { "0" } else { "1" }

$ContainerScript = @'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  wget xz-utils make cmake perl ca-certificates python3 g++ libssl-dev flex bison git p7zip-full

ARM_TOOLCHAIN_VERSION=15.3.rel2
ARM_TOOLCHAIN_SHA256=463df9665c246fa13bab2102115bbf83440661b6eb70b44f3dfd4712ca30037a
TC_BASE="arm-gnu-toolchain-${ARM_TOOLCHAIN_VERSION}-x86_64-arm-none-eabi"
TC_DIR=/opt/arm-gnu-toolchain
# The manifest names the installed release, so a volume holding an older
# toolchain is replaced instead of reused.
if [ ! -f "$TC_DIR/${ARM_TOOLCHAIN_VERSION}-x86_64-arm-none-eabi-manifest.txt" ]; then
  rm -rf "$TC_DIR"/*
  wget -q \
    "https://gitlab.arm.com/api/v4/projects/tooling%2Fgnu-toolchains-for-arm/packages/generic/gnu-toolchain/${ARM_TOOLCHAIN_VERSION}/${TC_BASE}.tar.xz" \
    -O /tmp/toolchain.tar.xz
  echo "${ARM_TOOLCHAIN_SHA256}  /tmp/toolchain.tar.xz" | sha256sum -c -
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
'@

$TempScript = Join-Path $RepoRoot `
  (".zz9000-build-firmware-" + [guid]::NewGuid().ToString("N") + ".sh")
[System.IO.File]::WriteAllText(
  $TempScript,
  ($ContainerScript -replace "`r`n", "`n" -replace "`r", "`n"),
  [System.Text.Encoding]::ASCII
)

try {
  docker run --rm `
    -v "${RepoRoot}:/work" `
    -v "zz9000-arm-toolchain:/opt/arm-gnu-toolchain" `
    -v "zz9000-bootgen:/opt/bootgen" `
    -v "${TempScript}:/tmp/build-firmware-docker.sh:ro" `
    -w /work `
    -e "CLEAN=$CleanValue" `
    -e "BOOTIMAGE=$BootimageValue" `
    -e "OUTPUT=$Output" `
    -e "EXTRA_CFLAGS=$ExtraCFlags" `
    $Image bash /tmp/build-firmware-docker.sh
  if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
  }
} finally {
  Remove-Item -LiteralPath $TempScript -ErrorAction SilentlyContinue
}
