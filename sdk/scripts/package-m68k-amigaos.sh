#!/bin/sh
# Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
# SPDX-License-Identifier: GPL-3.0-or-later
set -e

SKIP_BUILD=0
OUTPUT_DIR="build/package/amigaos3"

while [ "$#" -gt 0 ]; do
  case "$1" in
    --skip-build)
      SKIP_BUILD=1
      ;;
    --output-dir)
      shift
      OUTPUT_DIR="$1"
      ;;
    *)
      echo "usage: $0 [--skip-build] [--output-dir build/package/amigaos3]" >&2
      exit 2
      ;;
  esac
  shift
done

REPO_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
if ! command -v realpath >/dev/null 2>&1; then
  echo "realpath is required to validate the package output path" >&2
  exit 2
fi

SAFE_ROOT="$(realpath -m "$REPO_ROOT/build/package")"
case "$OUTPUT_DIR" in
  /*)
    PACKAGE_ROOT="$(realpath -m "$OUTPUT_DIR")"
    ;;
  *)
    PACKAGE_ROOT="$(realpath -m "$REPO_ROOT/$OUTPUT_DIR")"
    ;;
esac
case "$PACKAGE_ROOT" in
  "$SAFE_ROOT"/*)
    ;;
  *)
    echo "output directory must stay under build/package" >&2
    exit 2
    ;;
esac
PACKAGE_PARENT="$(dirname -- "$PACKAGE_ROOT")"

if [ "$SKIP_BUILD" -eq 0 ]; then
  "$REPO_ROOT/scripts/build-m68k-amigaos.sh"
fi

mkdir -p "$PACKAGE_PARENT"
rm -rf "$PACKAGE_ROOT"
mkdir -p \
  "$PACKAGE_ROOT/Libs" \
  "$PACKAGE_ROOT/Classes/DataTypes" \
  "$PACKAGE_ROOT/Storage/DataTypes" \
  "$PACKAGE_ROOT/C" \
  "$PACKAGE_ROOT/Developer/Include/zz9k" \
  "$PACKAGE_ROOT/Developer/Include/libraries" \
  "$PACKAGE_ROOT/Developer/Include/proto" \
  "$PACKAGE_ROOT/Developer/Include/clib" \
  "$PACKAGE_ROOT/Developer/Include/inline" \
  "$PACKAGE_ROOT/Developer/Include/pragmas" \
  "$PACKAGE_ROOT/Developer/Include/fd" \
  "$PACKAGE_ROOT/Developer/FD" \
  "$PACKAGE_ROOT/Docs" \
  "$PACKAGE_ROOT/Examples" \
  "$PACKAGE_ROOT/Archives"

copy_one() {
  cp "$REPO_ROOT/$1" "$PACKAGE_ROOT/$2"
}

copy_tree() {
  cp -R "$REPO_ROOT/$1"/. "$PACKAGE_ROOT/$2"
}

# Amiga-side manuals ship as AmigaGuide databases generated from the
# canonical Markdown sources: raw .md has no reader on AmigaOS. The
# converter is stdlib-only Python and this script runs on the host,
# never inside the m68k Docker container. Cross-document links resolve
# to the sibling guides in GUIDE_SPECS, so the shipped doc set and this
# list must stay in sync.
GUIDE_SPECS="README.md:ZZ9000-SDK \
docs/zz9k-library.md:zz9k-library \
docs/zz9k-zorro2-services.md:zz9k-zorro2-services \
docs/zz9k-picture-datatype.md:zz9k-picture-datatype \
docs/zzplay.md:ZZPlay \
docs/zz9k-amissl-provider.md:zz9k-amissl-provider"
GUIDE_SIBLINGS=""
for spec in $GUIDE_SPECS; do
  GUIDE_SIBLINGS="$GUIDE_SIBLINGS --sibling ${spec##*:}.guide"
done
make_guide() {
  src=${1%%:*}
  name=${1##*:}
  # shellcheck disable=SC2086
  python3 "$REPO_ROOT/scripts/md2guide.py" \
    --name "$name" $GUIDE_SIBLINGS \
    "$REPO_ROOT/$src" "$PACKAGE_ROOT/Docs/$name.guide"
}

decode_base64_file() {
  if base64 -d "$REPO_ROOT/$1" > "$PACKAGE_ROOT/$2" 2>/dev/null; then
    return
  fi
  if base64 -D "$REPO_ROOT/$1" > "$PACKAGE_ROOT/$2" 2>/dev/null; then
    return
  fi
  if command -v openssl >/dev/null 2>&1; then
    openssl base64 -d -in "$REPO_ROOT/$1" -out "$PACKAGE_ROOT/$2"
    return
  fi
  echo "base64 decoder is required to package DataTypes descriptors" >&2
  exit 2
}

copy_one "build/zz9k.library" "Libs/zz9k.library"
copy_one "build/mpega.library" "Libs/mpega.library"

copy_one "build/zz9k-info" "C/zz9k-info"
copy_one "build/zz9k-services" "C/zz9k-services"
copy_one "build/zz9k-fabriclease" "C/zz9k-fabriclease"
copy_one "build/zz9k-bench" "C/zz9k-bench"
copy_one "build/zz9k-hash" "C/zz9k-hash"
copy_one "build/zz9k-inflate" "C/zz9k-inflate"
copy_one "build/zz9k-archive" "C/zz9k-archive"
copy_one "build/zz9k-irqtest" "C/zz9k-irqtest"
copy_one "build/zz9k-chacha" "C/zz9k-chacha"
copy_one "build/zz9k-aead" "C/zz9k-aead"
copy_one "build/zz9k-cryptobench" "C/zz9k-cryptobench"
copy_one "build/zz9k-m68kbench" "C/zz9k-m68kbench"
copy_one "build/zz9k-mp3" "C/zz9k-mp3"
copy_one "build/zzplay" "C/ZZPlay"
copy_one "amiga/icons/zzplay.info" "C/ZZPlay.info"
copy_one "amiga/icons/zzplay-project.info" "Docs/ZZPlay-project.info"
copy_one "build/zz9k-mpega-smoke" "C/zz9k-mpega-smoke"
copy_one "build/zz9k-jpeg" "C/zz9k-jpeg"
copy_one "build/zz9k-png" "C/zz9k-png"
copy_one "build/zz9k-view" "C/zz9k-view"
copy_one "build/zz9k-dtprobe" "C/zz9k-dtprobe"
copy_one "build/zz9k-picture.datatype" \
  "Classes/DataTypes/zz9k-picture.datatype"
decode_base64_file "amiga/datatypes/descriptors/ZZ9000-JPEG.b64" \
  "Storage/DataTypes/ZZ9000-JPEG"
decode_base64_file "amiga/datatypes/descriptors/ZZ9000-PNG.b64" \
  "Storage/DataTypes/ZZ9000-PNG"
copy_one "amiga/datatypes/descriptors/ZZ9000-JPEG.info" \
  "Storage/DataTypes/ZZ9000-JPEG.info"
copy_one "amiga/datatypes/descriptors/ZZ9000-PNG.info" \
  "Storage/DataTypes/ZZ9000-PNG.info"
copy_one "build/zz9k-smoke" "C/zz9k-smoke"
copy_one "build/zz9k-surface-info" "C/zz9k-surface-info"
copy_one "build/zz9k-fbtest" "C/zz9k-fbtest"
copy_one "build/zz9k-scaletest" "C/zz9k-scaletest"
copy_one "build/zz9k-surfaceops" "C/zz9k-surfaceops"
copy_one "build/zz9k-probe" "C/zz9k-probe"
copy_one "build/zz9k-step" "C/zz9k-step"
copy_one "build/zz9k-libtest" "C/zz9k-libtest"
copy_one "build/zz9k-libasync" "C/zz9k-libasync"
copy_one "build/zz9k-libbatch" "C/zz9k-libbatch"
copy_one "build/zz9k-libcancel" "C/zz9k-libcancel"
copy_one "build/zz9k-libwait" "C/zz9k-libwait"
copy_one "build/zz9k-libwaitbatch" "C/zz9k-libwaitbatch"
copy_one "build/zz9k-libevent" "C/zz9k-libevent"
copy_one "build/zz9k-libeventbatch" "C/zz9k-libeventbatch"
copy_one "build/zz9k-libeventcb" "C/zz9k-libeventcb"
copy_one "build/zz9k-libeventlife" "C/zz9k-libeventlife"
copy_one "build/zz9k-libeventports" "C/zz9k-libeventports"
copy_one "build/zz9k-libeventcancel" "C/zz9k-libeventcancel"
copy_one "build/zz9k-libdecode" "C/zz9k-libdecode"
copy_one "build/zz9k-libsmoke" "C/zz9k-libsmoke"
copy_one "build/zz9k-library-demo" "C/zz9k-library-demo"
copy_one "build/zz9k-jpeg-stream-demo" "C/zz9k-jpeg-stream-demo"
copy_one "build/zz9k-typed-decode-demo" "C/zz9k-typed-decode-demo"
copy_one "build/zz9k-crypto-demo" "C/zz9k-crypto-demo"

copy_tree "include/zz9k" "Developer/Include/zz9k"
copy_tree "host/include/zz9k" "Developer/Include/zz9k"
copy_tree "amiga/include/zz9k" "Developer/Include/zz9k"
copy_tree "amiga/include/libraries" "Developer/Include/libraries"
copy_tree "amiga/include/proto" "Developer/Include/proto"
copy_tree "amiga/include/clib" "Developer/Include/clib"
copy_tree "amiga/include/inline" "Developer/Include/inline"
copy_tree "amiga/include/pragmas" "Developer/Include/pragmas"
copy_one "amiga/fd/zz9k_lib.fd" "Developer/FD/zz9k_lib.fd"
copy_one "amiga/fd/mpega_lib.fd" "Developer/FD/mpega_lib.fd"
copy_one "amiga/fd/mpega.fd" "Developer/FD/mpega.fd"
copy_one "amiga/fd/mpega.fd" "Developer/Include/fd/mpega.fd"

for spec in $GUIDE_SPECS; do
  make_guide "$spec"
done
decode_base64_file "tests/fixtures/archives/split-deflate.7z.b64" \
  "Archives/split-deflate.7z"
decode_base64_file "tests/fixtures/archives/split-lzma.7z.b64" \
  "Archives/split-lzma.7z"
decode_base64_file "tests/fixtures/archives/split-lzma2.7z.b64" \
  "Archives/split-lzma2.7z"
copy_one "examples/amiga-library/zz9k-library-demo.c" \
  "Examples/zz9k-library-demo.c"
copy_one "examples/amiga-jpeg-stream/zz9k-jpeg-stream-demo.c" \
  "Examples/zz9k-jpeg-stream-demo.c"
copy_one "examples/amiga-typed-decode/zz9k-typed-decode-demo.c" \
  "Examples/zz9k-typed-decode-demo.c"
copy_one "examples/amiga-crypto/zz9k-crypto-demo.c" \
  "Examples/zz9k-crypto-demo.c"

(
  cd "$PACKAGE_ROOT"
  find . -type f ! -name MANIFEST.sha256 |
    sed 's#^\./##' |
    LC_ALL=C sort |
    while IFS= read -r file; do
      if command -v sha256sum >/dev/null 2>&1; then
        hash="$(sha256sum "$file" | awk '{print $1}')"
      else
        hash="$(shasum -a 256 "$file" | awk '{print $1}')"
      fi
      printf '%s  %s\n' "$hash" "$file"
    done
) > "$PACKAGE_ROOT/MANIFEST.sha256"

echo "Packaged AmigaOS 3 SDK to $PACKAGE_ROOT"
