#!/bin/sh
# Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Compile the SDK sources that the AmiSSL provider build links
# (host/src/zz9k_host.c, tools/zz9k-crypto-soft.c) with the compiler that
# build uses: the adtools GCC 2.95.3 of sdk/integration/amissl/Dockerfile.
# Everything the SDK compiles with GCC 16 is also parsed by 2.95.3 here, so a
# header construct 2.95.3 rejects (named asm operands, // in headers, GCC 3+
# extensions) fails a pull request instead of the master-only AmiSSL build
# (.github/workflows/build-amissl-provider.yml, ~15 minutes per target).
#
# The provider sources themselves (amiga/provider/*.c) need OpenSSL headers
# from an AmiSSL checkout and are only covered by the full build.
#
# The source tree is mounted read-only and copied inside the container.
set -eu

SDK="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
IMAGE=zz9000-amissl-adtools

docker build -q -t "$IMAGE" "$SDK/integration/amissl" >/dev/null

# Same flags as the AmiSSL Makefile (CFLAGS + ZZ9000_CFLAGS in
# integration/amissl/amissl-zz9000.patch), both CPU targets.
MSYS_NO_PATHCONV=1 docker run --rm -v "$SDK:/sdk-src:ro" "$IMAGE" sh -ec '
cp -r /sdk-src /build-sdk
S=/build-sdk
m68k-amigaos-gcc --version | head -n 1
for cpu in "-m68020" "-m68060 -msoft-float"; do
  for src in host/src/zz9k_host.c tools/zz9k-crypto-soft.c; do
    echo "CC ($cpu) $src"
    m68k-amigaos-gcc -mcrt=clib2 $cpu -W -Wall -Wno-unused -O1 \
      -D__NO_NET_API -DB_ENDIAN -DOPENSSL_SYS_AMIGA -D__C_MACROS__ \
      -DOPENSSL_NO_STDIO -DNO_CHMOD -DNO_INLINE_VARARGS -D__amigaos3__ -DNDEBUG \
      -DZZ9K_PROVIDER_OFFLOAD \
      -I$S/include -I$S/host/include -I$S/amiga/include -I$S/tools \
      -I$S/amiga/provider -c "$S/$src" -o /tmp/check.o
  done
done
echo "amissl sources: OK"
'
