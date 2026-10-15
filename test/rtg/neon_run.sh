#!/bin/bash
# RTG regression suite on the firmware's NEON code paths.
#
# gfx.c only builds its NEON kernels for ARM, so the host build
# (make -C test/rtg test) checks the scalar paths alone. This compiles the
# same harness for armhf with the firmware's CPU flags and runs it under
# qemu-arm in Docker.
#
# Usage: test/rtg/neon_run.sh
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"

MSYS_NO_PATHCONV=1 docker run --rm -v "$repo:/src:ro" ubuntu:24.04 bash -c '
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq gcc-arm-linux-gnueabihf libc6-dev-armhf-cross \
    qemu-user >/dev/null
F=/src/ZZ9000_proto.sdk/ZZ9000OS/src
# The firmware CPU flags and default optimisation (ZZ9000OS/Makefile).
FW="-mcpu=cortex-a9 -marm -mfpu=neon -mfloat-abi=hard -O2"
arm-linux-gnueabihf-gcc $FW -D_POSIX_C_SOURCE=200809L -I$F \
    -Wall -Wextra -Wno-int-to-pointer-cast -Wno-unused-parameter \
    -Wno-implicit-fallthrough -Wno-type-limits -Wno-unused-but-set-variable \
    -o /tmp/rtg_regression /src/test/rtg/rtg_regression.c $F/gfx.c -lm -static
qemu-arm /tmp/rtg_regression
'
