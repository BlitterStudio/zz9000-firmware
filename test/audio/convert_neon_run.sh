#!/bin/bash
# NEON rate-converter exactness test (audio_convert_exact_test.c).
#
# The NEON kernel only builds for ARM, so the test is compiled for armhf
# with the firmware's CPU flags and run under qemu-arm in Docker. The host
# build of the same test (make -C test/audio) covers the scalar path.
#
# Usage: test/audio/convert_neon_run.sh
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
arm-linux-gnueabihf-gcc $FW -std=c99 -Wall -Wextra -Werror -I$F \
    -o /tmp/audio_convert_exact_test \
    /src/test/audio/audio_convert_exact_test.c $F/audio_convert.c \
    $F/audio_convert_tables.c -static
qemu-arm /tmp/audio_convert_exact_test
'
