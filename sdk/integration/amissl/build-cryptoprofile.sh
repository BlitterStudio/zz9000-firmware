#!/bin/sh
# Build zz9k-cryptoprofile: a pure AmiSSL client (m68k AmigaOS) that times the
# TLS handshake crypto primitives through the installed amissl.library, offload
# vs software. It does NOT compile in the ZZ9000 provider — the drop-in library
# already registers it and sets "?provider=zz9000" (see
# docs/zz9k-amissl-provider.md), so the default context IS the offload path a
# real browser uses. Run inside the sacredbanana/amiga-compiler:m68k-amigaos
# image, which ships the AmiSSL SDK.
set -e
cd /sdk
OUT=/sdk/integration/amissl/work/out
mkdir -p "$OUT"
cd "$OUT"

CC=m68k-amigaos-gcc
CF="-noixemul -O2"

echo ">> compiling zz9k-cryptoprofile (force-include proto/amissl.h)"
$CC $CF -include proto/amissl.h -c /sdk/tools/zz9k-cryptoprofile.c \
    -o zz9k_cryptoprofile.o

echo ">> linking (manual AmiSSL model — the program opens the bases)"
$CC -noixemul -o zz9k-cryptoprofile zz9k_cryptoprofile.o

echo ">> done: $OUT/zz9k-cryptoprofile"
ls -l "$OUT/zz9k-cryptoprofile"
