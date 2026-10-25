#!/bin/bash
# NEON IDCT/motion-compensation exactness test (mpeg2_neon_test.c).
#
# The kernels only exist on ARM, so the test builds for armhf with the
# firmware's codec flags and runs under qemu-arm in Docker. Needs Docker
# and the libmpeg2 sources fetched by build_dvd_codecs.sh (any host test
# run fetches them). Extra arguments are MPEG Program Stream files, under
# the repository root, to compare as well.
#
# Usage: test/mpeg2_neon/run.sh [clip.mpg ...]
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
deps="$repo/ZZ9000_proto.sdk/ZZ9000OS/build/deps"
# The runner compiles the fetched (and patched) sources and the host
# configuration headers that the host codec build generates.
if [ ! -f "$deps/libmpeg2/0.5.1/src/libmpeg2/decode.c" ] ||
   [ ! -f "$deps/a52dec/0.7.4/host/include/config.h" ]; then
    (cd "$repo" && bash ./build_dvd_codecs.sh --host)
fi

clips=()
for clip in "$@"; do
    path="$(cd "$(dirname "$clip")" && pwd)/$(basename "$clip")"
    case "$path" in
    "$repo"/*) clips+=("/src/${path#"$repo"/}") ;;
    *) echo "clip must be under $repo: $clip" >&2; exit 2 ;;
    esac
done

MSYS_NO_PATHCONV=1 docker run --rm -v "$repo:/src:ro" ubuntu:24.04 bash -c '
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq gcc-arm-linux-gnueabihf libc6-dev-armhf-cross \
    qemu-user >/dev/null
OS=/src/ZZ9000_proto.sdk/ZZ9000OS
F=$OS/src
W=$OS/modules/webm
M=$OS/build/deps/libmpeg2/0.5.1/src
A=$OS/build/deps/a52dec/0.7.4
CC=arm-linux-gnueabihf-gcc
# The firmware codec and backend flags (build_dvd_codecs.sh, Makefile).
FW="-mcpu=cortex-a9 -marm -mfpu=neon -mfloat-abi=hard -O3 -fno-strict-aliasing"
mkdir -p /b/inc && cd /b
printf "#define ATTRIBUTE_ALIGNED_MAX 16\n#define HAVE_BUILTIN_EXPECT 1\n" \
    > inc/config.h
# build_dvd_codecs.sh has already applied its short-enum patch to src.
for tu in alloc cpu_accel cpu_state decode header idct motion_comp slice; do
    $CC $FW -I$M/include -Iinc -c $M/libmpeg2/$tu.c -o m_$tu.o
done
for tu in bitstream bit_allocate downmix imdct parse; do
    $CC $FW -I$A/src/include -I$A/src/liba52 -I$A/host/include \
        -c $A/src/liba52/$tu.c -o a_$tu.o
done
$CC $FW -std=c99 -Wall -Wextra -DSDK_VIDEO_HOST_TEST=1 \
    -DSDK_MEDIA_HOST_TEST=1 -I$F -I$M/include -I$A/host/include \
    -o mpeg2_neon_test \
    /src/test/mpeg2_neon/mpeg2_neon_test.c $F/sdk_mpeg2_neon.c \
    $F/sdk_video_mpeg2.c $W/webm_parse.c $W/webm_window.c \
    $F/sdk_dvd_ps.c $F/sdk_dvd_ac3.c \
    $F/sdk_dvd_lpcm.c $F/mp3/decode_mp3.c $F/sdk_media_timeline.c \
    $F/sdk_media_profile.c $F/sdk_video_yuy2.c m_*.o a_*.o -lm -static
qemu-arm ./mpeg2_neon_test "$@"
' bash "${clips[@]}"
