#!/bin/sh
# Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
# SPDX-License-Identifier: GPL-3.0-or-later
set -e

IMAGE="${IMAGE:-sacredbanana/amiga-compiler:m68k-amigaos}"
REPO_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

# Run as root inside the container: the toolchain NDK headers under
# /opt/m68k-amigaos are not world-readable, so a non-root --user cannot
# compile. Pass the invoking host user/group ids so the container can hand the
# build outputs back at the end (below) — otherwise the root-owned build/ tree
# blocks the host-side packaging step from writing build/package on Linux/CI.
docker run --rm -v "$REPO_ROOT:/work" -w /work \
  -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" "$IMAGE" sh -c '
set -e
mkdir -p build/m68k
CFLAGS="-noixemul -Os -s -Iinclude -Ihost/include"
LIBCFLAGS="$CFLAGS -Iamiga/include"

m68k-amigaos-gcc $CFLAGS -c host/src/zz9k_host.c -o build/m68k/zz9k_host.o
m68k-amigaos-gcc $LIBCFLAGS -c amiga/src/zz9k_library.c -o build/m68k/zz9k_library.o
m68k-amigaos-gcc $LIBCFLAGS -c amiga/src/zz9k_library_resident.c -o build/m68k/zz9k_library_resident.o
m68k-amigaos-gcc $CFLAGS -Itools -c tools/zz9k-fb-common.c -o build/m68k/zz9k-fb-common.o
m68k-amigaos-gcc $CFLAGS -Itools -c tools/zz9k-image-window.c -o build/m68k/zz9k-image-window.o
m68k-amigaos-gcc $CFLAGS -Itools -c tools/zz9k-picture-viewer.c -o build/m68k/zz9k-picture-viewer.o
m68k-amigaos-gcc $CFLAGS -Itools -DZZ9K_JPEG_NO_MAIN=1 \
  -c tools/zz9k-jpeg.c -o build/m68k/zz9k-jpeg-view.o
m68k-amigaos-gcc $CFLAGS -Itools -DZZ9K_PNG_NO_MAIN=1 \
  -c tools/zz9k-png.c -o build/m68k/zz9k-png-view.o
m68k-amigaos-gcc $CFLAGS -Itools -DZZ9K_IMAGE_WINDOW_NO_UI=1 \
  -ffunction-sections -fdata-sections \
  -c tools/zz9k-image-window.c -o build/m68k/zz9k-image-window-resident.o
for src in bitio crcio dhuf extract huf larc maketbl maketree shuf slide zz9k_lha_unix zz9k_lha_unix_support; do
  m68k-amigaos-gcc $CFLAGS -DHAVE_CONFIG_H=1 -Itools/lha-unix \
    -c tools/lha-unix/$src.c -o build/m68k/lha-$src.o
done
LHA_OBJS="build/m68k/lha-bitio.o build/m68k/lha-crcio.o build/m68k/lha-dhuf.o build/m68k/lha-extract.o build/m68k/lha-huf.o build/m68k/lha-larc.o build/m68k/lha-maketbl.o build/m68k/lha-maketree.o build/m68k/lha-shuf.o build/m68k/lha-slide.o build/m68k/lha-zz9k_lha_unix.o build/m68k/lha-zz9k_lha_unix_support.o"
ZZPLAY_SOURCES="tools/zzplay-ahi.c tools/zzplay-audio.c tools/zzplay-audio-clock.c tools/zzplay-ax.c tools/zzplay-core.c tools/zzplay-geometry.c tools/zzplay-launch.c tools/zzplay-media.c tools/zzplay-mhi.c tools/zzplay-mp3.c tools/zzplay-mp3-transport.c tools/zzplay-options.c tools/zzplay-path.c tools/zzplay-probe.c tools/zzplay-stats.c tools/zzplay-statuswin.c tools/zzplay-stream.c tools/zzplay-sync.c tools/zzplay-video.c tools/zzplay.c"

m68k-amigaos-gcc -noixemul -nostartfiles -Os -s -Iinclude -Ihost/include -Iamiga/include \
  build/m68k/zz9k_library_resident.o build/m68k/zz9k_library.o \
  build/m68k/zz9k_host.o -o build/zz9k.library
m68k-amigaos-gcc -noixemul -nostartfiles -Os -s -Iinclude -Ihost/include -Iamiga/include \
  amiga/mpega/mpega_resident.c -o build/mpega.library

m68k-amigaos-gcc $LIBCFLAGS tools/zz9k-libtest.c -o build/zz9k-libtest
m68k-amigaos-gcc $LIBCFLAGS tools/zz9k-libasync.c -o build/zz9k-libasync
m68k-amigaos-gcc $LIBCFLAGS tools/zz9k-libbatch.c -o build/zz9k-libbatch
m68k-amigaos-gcc $LIBCFLAGS tools/zz9k-libcancel.c -o build/zz9k-libcancel
m68k-amigaos-gcc $LIBCFLAGS tools/zz9k-libwait.c -o build/zz9k-libwait
m68k-amigaos-gcc $LIBCFLAGS tools/zz9k-libwaitbatch.c -o build/zz9k-libwaitbatch
m68k-amigaos-gcc $LIBCFLAGS tools/zz9k-libevent.c -o build/zz9k-libevent
m68k-amigaos-gcc $LIBCFLAGS tools/zz9k-libeventbatch.c -o build/zz9k-libeventbatch
m68k-amigaos-gcc $LIBCFLAGS tools/zz9k-libeventcb.c -o build/zz9k-libeventcb
m68k-amigaos-gcc $LIBCFLAGS tools/zz9k-libeventlife.c -o build/zz9k-libeventlife
m68k-amigaos-gcc $LIBCFLAGS tools/zz9k-libeventports.c -o build/zz9k-libeventports
m68k-amigaos-gcc $LIBCFLAGS tools/zz9k-libeventcancel.c -o build/zz9k-libeventcancel
m68k-amigaos-gcc $LIBCFLAGS tools/zz9k-libdecode.c -o build/zz9k-libdecode
m68k-amigaos-gcc $LIBCFLAGS -Itools build/m68k/zz9k-fb-common.o \
  tools/zz9k-libsmoke.c -o build/zz9k-libsmoke
m68k-amigaos-gcc $LIBCFLAGS examples/amiga-library/zz9k-library-demo.c \
  -o build/zz9k-library-demo
m68k-amigaos-gcc $LIBCFLAGS examples/amiga-jpeg-stream/zz9k-jpeg-stream-demo.c \
  -o build/zz9k-jpeg-stream-demo
m68k-amigaos-gcc $LIBCFLAGS examples/amiga-typed-decode/zz9k-typed-decode-demo.c \
  -o build/zz9k-typed-decode-demo
m68k-amigaos-gcc $LIBCFLAGS examples/amiga-crypto/zz9k-crypto-demo.c \
  -o build/zz9k-crypto-demo
m68k-amigaos-gcc $CFLAGS build/m68k/zz9k_host.o tools/zz9k-info.c -o build/zz9k-info
m68k-amigaos-gcc $CFLAGS build/m68k/zz9k_host.o tools/zz9k-services.c -o build/zz9k-services
m68k-amigaos-gcc $CFLAGS build/m68k/zz9k_host.o tools/zz9k-fabriclease.c -o build/zz9k-fabriclease
m68k-amigaos-gcc $CFLAGS build/m68k/zz9k_host.o tools/zz9k-bench.c -o build/zz9k-bench
m68k-amigaos-gcc $CFLAGS build/m68k/zz9k_host.o tools/zz9k-hash.c -o build/zz9k-hash
m68k-amigaos-gcc $CFLAGS build/m68k/zz9k_host.o tools/zz9k-inflate.c -o build/zz9k-inflate
m68k-amigaos-gcc $CFLAGS -Itools/lha-unix build/m68k/zz9k_host.o $LHA_OBJS tools/zz9k-archive.c -o build/zz9k-archive
m68k-amigaos-gcc $CFLAGS build/m68k/zz9k_host.o tools/zz9k-irqtest.c -o build/zz9k-irqtest
m68k-amigaos-gcc $CFLAGS build/m68k/zz9k_host.o tools/zz9k-chacha.c -o build/zz9k-chacha
m68k-amigaos-gcc $CFLAGS build/m68k/zz9k_host.o tools/zz9k-aead.c -o build/zz9k-aead
m68k-amigaos-gcc $CFLAGS -Itools build/m68k/zz9k_host.o tools/zz9k-crypto-soft.c tools/zz9k-cryptobench.c -o build/zz9k-cryptobench
m68k-amigaos-gcc $CFLAGS -Itools tools/zz9k-m68kbench.c -o build/zz9k-m68kbench
m68k-amigaos-gcc $CFLAGS build/m68k/zz9k_host.o tools/zzplay-mp3-transport.c tools/zz9k-mp3.c -o build/zz9k-mp3
m68k-amigaos-gcc $CFLAGS -Itools build/m68k/zz9k_host.o $ZZPLAY_SOURCES -o build/zzplay
m68k-amigaos-gcc $LIBCFLAGS tools/zz9k-mpega-smoke.c -o build/zz9k-mpega-smoke
m68k-amigaos-gcc $CFLAGS -Itools build/m68k/zz9k_host.o build/m68k/zz9k-fb-common.o build/m68k/zz9k-image-window.o build/m68k/zz9k-picture-viewer.o tools/zz9k-jpeg.c -o build/zz9k-jpeg
m68k-amigaos-gcc $CFLAGS -Itools build/m68k/zz9k_host.o build/m68k/zz9k-fb-common.o build/m68k/zz9k-image-window.o build/m68k/zz9k-picture-viewer.o tools/zz9k-png.c -o build/zz9k-png
m68k-amigaos-gcc $CFLAGS -Itools build/m68k/zz9k_host.o build/m68k/zz9k-fb-common.o build/m68k/zz9k-image-window.o build/m68k/zz9k-picture-viewer.o build/m68k/zz9k-jpeg-view.o build/m68k/zz9k-png-view.o tools/zz9k-view.c -o build/zz9k-view
m68k-amigaos-gcc $LIBCFLAGS tools/zz9k-dtprobe.c -o build/zz9k-dtprobe
m68k-amigaos-gcc -noixemul -nostartfiles -Os -s -Iinclude -Ihost/include -Iamiga/include -Itools \
  build/m68k/zz9k_host.o build/m68k/zz9k-fb-common.o \
  build/m68k/zz9k-image-window-resident.o \
  amiga/datatypes/zz9k_picture_datatype.c -o build/zz9k-picture.datatype
m68k-amigaos-gcc $CFLAGS build/m68k/zz9k_host.o tools/zz9k-smoke.c -o build/zz9k-smoke
m68k-amigaos-gcc $CFLAGS build/m68k/zz9k_host.o tools/zz9k-surface-info.c -o build/zz9k-surface-info
m68k-amigaos-gcc $CFLAGS build/m68k/zz9k_host.o tools/zz9k-palette.c -o build/zz9k-palette
m68k-amigaos-gcc $CFLAGS -Itools build/m68k/zz9k_host.o build/m68k/zz9k-fb-common.o \
  tools/zz9k-fbtest.c -o build/zz9k-fbtest
m68k-amigaos-gcc $CFLAGS -Itools build/m68k/zz9k_host.o build/m68k/zz9k-fb-common.o \
  tools/zz9k-scaletest.c -o build/zz9k-scaletest
m68k-amigaos-gcc $CFLAGS -Itools build/m68k/zz9k_host.o build/m68k/zz9k-fb-common.o \
  build/m68k/zz9k-image-window.o \
  tools/zz9k-surfaceops.c -o build/zz9k-surfaceops
m68k-amigaos-gcc $CFLAGS tools/zz9k-probe.c -o build/zz9k-probe
m68k-amigaos-gcc $CFLAGS tools/zz9k-step.c -o build/zz9k-step

# Hand the root-owned build outputs back to the invoking host user so the
# host-side packaging step (and any later cleanup) can write under build/.
if [ -n "${HOST_UID:-}" ]; then chown -R "$HOST_UID:$HOST_GID" build || true; fi
'
