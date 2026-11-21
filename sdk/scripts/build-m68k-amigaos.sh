#!/bin/sh
# Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
# SPDX-License-Identifier: GPL-3.0-or-later
set -e

# GCC 16. The tag follows release candidates, so the digest pins the
# compiler; card runs of it happen before releases.
IMAGE="${IMAGE:-amigadev/crosstools:m68k-amigaos@sha256:9391792c327c1058f04cb0a0e5e67bbf5504e9b696f486f6fbfb214bf6ca2d64}"
REPO_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

# aar.library is built here from the aar checkout pinned in sdk/AAR_REF: the
# portable core, the AmigaOS shell and the probe (client/src/aar_core.c,
# aar_shell.c, aar_probe.c, MIT) linked with the ZZ9000 platform layer of this
# tree (amiga/src/aar_zz9000.c, GPL), which includes the private wire
# zzapp_proto.h of the firmware tree. aar-run and libaarserve.a are built from
# the same checkout (client/run, client/serve, MIT) and do not link the
# platform layer. The pinned aar checkout comes from
# scripts/fetch-aar.sh (or AAR_DIR=<your checkout>); the wire header is staged
# under build/.
AAR_DIR="${AAR_DIR:-$REPO_ROOT/aar}"
if [ ! -f "$AAR_DIR/include/aar/transport.h" ]; then
  sh "$REPO_ROOT/scripts/fetch-aar.sh" "$AAR_DIR"
fi
for f in aar_core.c aar_shell.c aar_probe.c aar_plat.h; do
  if [ ! -f "$AAR_DIR/client/src/$f" ]; then
    echo "build-m68k-amigaos: $AAR_DIR/client/src/$f is missing; sdk/AAR_REF must name an aar commit with the client library" >&2
    exit 1
  fi
done
for f in run/aar_run.c run/aar_run_zz9000.c run/aar-run.info run/README.md \
    serve/aar_serve.c serve/aar_serve_core.c include/libraries/aar_serve.h; do
  if [ ! -f "$AAR_DIR/client/$f" ]; then
    echo "build-m68k-amigaos: $AAR_DIR/client/$f is missing; sdk/AAR_REF must name an aar commit with aar-run and the serving loop" >&2
    exit 1
  fi
done
mkdir -p "$REPO_ROOT/build/stage"
cp "$REPO_ROOT/../ZZ9000_proto.sdk/ZZ9000OS/src/zzapp_proto.h" \
  "$REPO_ROOT/build/stage/zzapp_proto.h"

# Run as root inside the container: the toolchain NDK headers under
# /opt/m68k-amigaos are not world-readable, so a non-root --user cannot
# compile. Pass the invoking host user/group ids so the container can hand the
# build outputs back at the end (below) — otherwise the root-owned build/ tree
# blocks the host-side packaging step from writing build/package on Linux/CI.
# MSYS_NO_PATHCONV=1 keeps Git Bash from rewriting the container-absolute
# -w /work into a host path; the Docker daemon accepts the /d/... mount form.
MSYS_NO_PATHCONV=1 docker run --rm -v "$REPO_ROOT:/work" -v "$AAR_DIR:/aar:ro" -w /work \
  -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" "$IMAGE" sh -c '
set -e
mkdir -p build/m68k build/dtcode build/providers
CFLAGS="-fcommon -noixemul -Os -s -Werror -Iinclude -Ihost/include"
LIBCFLAGS="$CFLAGS -Iamiga/include"
STACKCFLAGS="$LIBCFLAGS -Wstack-usage=4000"
AARFLAGS="-Iamiga/src -Ibuild/stage -I/aar/client/src -I/aar/client/include -I/aar/include"

m68k-amigaos-gcc $CFLAGS -Iamiga/include -c host/src/zz9k_host.c -o build/m68k/zz9k_host.o
m68k-amigaos-gcc $CFLAGS -Iamiga/include -DZZ9K_HOST_LIBRARY_BUILD=1 \
  -c host/src/zz9k_host.c -o build/m68k/zz9k_host_library.o
m68k-amigaos-gcc $STACKCFLAGS -c amiga/src/zz9k_library.c -o build/m68k/zz9k_library.o
for src in aar_core aar_shell aar_probe; do
  m68k-amigaos-gcc $STACKCFLAGS $AARFLAGS -c /aar/client/src/$src.c -o build/m68k/$src.o
done
m68k-amigaos-gcc $STACKCFLAGS $AARFLAGS -c amiga/src/aar_zz9000.c -o build/m68k/aar_zz9000.o
m68k-amigaos-gcc $STACKCFLAGS -c amiga/src/zz9k_library_resident.c -o build/m68k/zz9k_library_resident.o
m68k-amigaos-gcc $CFLAGS -Itools -c tools/zz9k-fb-common.c -o build/m68k/zz9k-fb-common.o
m68k-amigaos-gcc $CFLAGS -Itools -c tools/zz9k-image-window.c -o build/m68k/zz9k-image-window.o
m68k-amigaos-gcc $CFLAGS -Itools -c tools/zz9k-picture-viewer.c -o build/m68k/zz9k-picture-viewer.o
m68k-amigaos-gcc $CFLAGS -Itools -DZZ9K_JPEG_NO_MAIN=1 \
  -c tools/zz9k-jpeg.c -o build/m68k/zz9k-jpeg-view.o
m68k-amigaos-gcc $CFLAGS -Itools -DZZ9K_PNG_NO_MAIN=1 \
  -c tools/zz9k-png.c -o build/m68k/zz9k-png-view.o
m68k-amigaos-gcc $CFLAGS -Itools -DZZ9K_WEBP_NO_MAIN=1 \
  -c tools/zz9k-webp.c -o build/m68k/zz9k-webp-view.o
m68k-amigaos-gcc $CFLAGS -Itools -DZZ9K_IMAGE_WINDOW_NO_UI=1 \
  -ffunction-sections -fdata-sections \
  -c tools/zz9k-image-window.c -o build/m68k/zz9k-image-window-resident.o
for src in bitio crcio dhuf extract huf larc maketbl maketree shuf slide zz9k_lha_unix zz9k_lha_unix_support; do
  m68k-amigaos-gcc $CFLAGS -DHAVE_CONFIG_H=1 -Itools/lha-unix \
    -c tools/lha-unix/$src.c -o build/m68k/lha-$src.o
done
LHA_OBJS="build/m68k/lha-bitio.o build/m68k/lha-crcio.o build/m68k/lha-dhuf.o build/m68k/lha-extract.o build/m68k/lha-huf.o build/m68k/lha-larc.o build/m68k/lha-maketbl.o build/m68k/lha-maketree.o build/m68k/lha-shuf.o build/m68k/lha-slide.o build/m68k/lha-zz9k_lha_unix.o build/m68k/lha-zz9k_lha_unix_support.o"
ZZPLAY_SOURCES="tools/zzplay/zzplay-ahi.c tools/zzplay/zzplay-ahiprefs.c tools/zzplay/zzplay-audio.c tools/zzplay/zzplay-audio-clock.c tools/zzplay/zzplay-ax.c tools/zzplay/zzplay-codec.c tools/zzplay/zzplay-codec-stream.c tools/zzplay/zzplay-controller.c tools/zzplay/zzplay-core.c tools/zzplay/zzplay-devices.c tools/zzplay/zzplay-files.c tools/zzplay/zzplay-formats.c tools/zzplay/zzplay-frame-clock.c tools/zzplay/zzplay-geometry.c tools/zzplay/zzplay-gui.c tools/zzplay/zzplay-launch.c tools/zzplay/zzplay-media.c tools/zzplay/zzplay-mhi.c tools/zzplay/zzplay-mpega.c tools/zzplay/zzplay-mp3.c tools/zzplay/zzplay-mp3-transport.c tools/zzplay/zzplay-options.c tools/zzplay/zzplay-path.c tools/zzplay/zzplay-playlist.c tools/zzplay/zzplay-prefs.c tools/zzplay/zzplay-probe.c tools/zzplay/zzplay-reader.c tools/zzplay/zzplay-stats.c tools/zzplay/zzplay-stream.c tools/zzplay/zzplay-sync.c tools/zzplay/zzplay-tags.c tools/zzplay/zzplay-video.c tools/zzplay/zzplay-webm.c tools/zzplay/zzplay.c"

ZZPLAY_SOURCES="$ZZPLAY_SOURCES tools/zzplay/zzplay-requester.c"
ZZPLAY_SOURCES="$ZZPLAY_SOURCES tools/zzplay/zzplay-rexx.c tools/zzplay/zzplay-rexx-commands.c"
ZZPLAY_SOURCES="$ZZPLAY_SOURCES tools/zzplay/zzplay-sys.c"
ZZPLAY_SOURCES="$ZZPLAY_SOURCES tools/zzplay/zzplay-http.c tools/zzplay/zzplay-httpfile.c tools/zzplay/zzplay-net.c tools/zzplay/zzplay-streamlist.c"
ZZPLAY_SOURCES="$ZZPLAY_SOURCES tools/zzplay/zzplay-xml.c tools/zzplay/zzplay-providers.c tools/zzplay/zzplay-host.c"
ZZPLAY_SOURCES="$ZZPLAY_SOURCES tools/zzplay/zzplay-browse.c tools/zzplay/zzplay-browse-model.c tools/zzplay/zzplay-browse-rows.c tools/zzplay/zzplay-browse-worker.c tools/zzplay/zzplay-browse-gather.c"
m68k-amigaos-gcc -noixemul -nostartfiles -Os -s -Iinclude -Ihost/include -Iamiga/include \
  build/m68k/zz9k_library_resident.o build/m68k/zz9k_library.o \
  build/m68k/zz9k_host_library.o -o build/zz9k.library
m68k-amigaos-gcc -noixemul -nostartfiles -Os -s -Iinclude -Ihost/include -Iamiga/include \
  amiga/mpega/mpega_resident.c -o build/mpega.library
m68k-amigaos-gcc -noixemul -nostartfiles -Os -s \
  build/m68k/aar_shell.o build/m68k/aar_core.o build/m68k/aar_probe.o \
  build/m68k/aar_zz9000.o -o build/aar.library

# aar-run, the reference launcher (client/run, MIT), and libaarserve.a, the
# portable serving loop it is built on (client/serve, MIT), from the same aar
# checkout. They are built with the flags of client/tests/m68k_check.sh in
# the aar repository and link no GPL code: the launcher calls the library
# through its vectors. The archive goes into the Developer directory of the
# package with the header libraries/aar_serve.h.
RUNFLAGS="-std=gnu11 -noixemul -m68030 -O2 -Wall -Wextra -Werror -I/aar/client/include -I/aar/include"
m68k-amigaos-gcc $RUNFLAGS -c /aar/client/serve/aar_serve.c -o build/m68k/aar_serve.o
m68k-amigaos-gcc $RUNFLAGS -c /aar/client/serve/aar_serve_core.c -o build/m68k/aar_serve_core.o
rm -f build/libaarserve.a
m68k-amigaos-ar rcs build/libaarserve.a build/m68k/aar_serve.o build/m68k/aar_serve_core.o
m68k-amigaos-gcc $RUNFLAGS /aar/client/run/aar_run.c \
  build/m68k/aar_serve.o build/m68k/aar_serve_core.o -lamiga -o build/aar-run

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
m68k-amigaos-gcc $STACKCFLAGS $AARFLAGS \
  tools/zz9k-app-echo-test.c -o build/zz9k-app-echo-test
m68k-amigaos-gcc $STACKCFLAGS $AARFLAGS \
  tools/zz9k-app-ringtest.c -o build/zz9k-app-ringtest
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
m68k-amigaos-gcc $CFLAGS build/m68k/zz9k_host.o tools/zzplay/zzplay-mp3-transport.c tools/zz9k-mp3.c -o build/zz9k-mp3
# swapstack.o grows the stack at startup to the __stack zzplay.c asks for:
# GadTools, ASL and AHI calls run on the calling task stack, and a Shell
# launch otherwise gets only its (often 4 KiB) default.
SWAPSTACK="$(m68k-amigaos-gcc -noixemul -print-file-name=swapstack.o)"
if [ ! -f "$SWAPSTACK" ]; then
  LIBNIX="$(m68k-amigaos-gcc -noixemul -print-file-name=libnix20.a)"
  m68k-amigaos-ar p "$LIBNIX" swapstack.o > build/m68k/swapstack.o
  SWAPSTACK="build/m68k/swapstack.o"
fi
m68k-amigaos-gcc $CFLAGS -Iamiga/include -Itools build/m68k/zz9k_host.o $ZZPLAY_SOURCES "$SWAPSTACK" -o build/zzplay
m68k-amigaos-gcc $LIBCFLAGS tools/zz9k-mpega-smoke.c -o build/zz9k-mpega-smoke
m68k-amigaos-gcc $CFLAGS -Itools build/m68k/zz9k_host.o build/m68k/zz9k-fb-common.o build/m68k/zz9k-image-window.o build/m68k/zz9k-picture-viewer.o tools/zz9k-jpeg.c -o build/zz9k-jpeg
m68k-amigaos-gcc $CFLAGS -Itools build/m68k/zz9k_host.o build/m68k/zz9k-fb-common.o build/m68k/zz9k-image-window.o build/m68k/zz9k-picture-viewer.o tools/zz9k-png.c -o build/zz9k-png
m68k-amigaos-gcc $CFLAGS -Itools build/m68k/zz9k_host.o build/m68k/zz9k-fb-common.o build/m68k/zz9k-image-window.o build/m68k/zz9k-picture-viewer.o build/m68k/zz9k-jpeg-view.o build/m68k/zz9k-png-view.o build/m68k/zz9k-webp-view.o tools/zz9k-view.c -o build/zz9k-view
m68k-amigaos-gcc $LIBCFLAGS tools/zz9k-dtprobe.c -o build/zz9k-dtprobe
m68k-amigaos-gcc -noixemul -nostartfiles -Os -s -Iinclude -Ihost/include -Iamiga/include -Itools \
  build/m68k/zz9k_host.o build/m68k/zz9k-fb-common.o \
  build/m68k/zz9k-image-window-resident.o \
  amiga/datatypes/zz9k_picture_datatype.c -o build/zz9k-picture.datatype
m68k-amigaos-gcc -noixemul -nostartfiles -Os -s -Iinclude -Ihost/include -Iamiga/include \
  build/m68k/zz9k_host.o amiga/datatypes/zz9k_sound_datatype.c \
  -o build/zz9k-sound.datatype
# Descriptor recognition hook: no startup code or C library; the entry stub
# must lead the first hunk. Embedded into ZZ9000-MP3 as its DTCD chunk.
m68k-amigaos-gcc -noixemul -nostartfiles -nostdlib -Os -s -fomit-frame-pointer \
  -Iinclude amiga/datatypes/zz9k_mp3_dthook_entry.S \
  amiga/datatypes/zz9k_mp3_dthook.c -o build/dtcode/zz9k-mp3-recog
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

# ZZPlay service modules: LoadSeg hunk files whose first code is the entry stub.
# No C library or startup code; the module calls through the host table.
for m in radio plex; do
  m68k-amigaos-gcc -noixemul -nostartfiles -nostdlib -ffreestanding \
    -fno-tree-loop-distribute-patterns -Os -s -fomit-frame-pointer \
    -Wall -Wextra -Werror -Wstack-usage=2048 \
    -Iinclude -Itools/zzplay \
    tools/zzplay/providers/zzprov_entry.S \
    tools/zzplay/providers/$m.c -lgcc -o build/providers/$m.zzprov
done

# Hand the root-owned build outputs back to the invoking host user so the
# host-side packaging step (and any later cleanup) can write under build/.
if [ -n "${HOST_UID:-}" ]; then chown -R "$HOST_UID:$HOST_GID" build || true; fi
'
