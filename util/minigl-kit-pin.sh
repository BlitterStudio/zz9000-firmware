#!/bin/sh
# Copyright (C) 2026, Dimitris Panokostas / BlitterStudio
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Firmware releases ship the MiniGL module kit (MINIGL.ZZM, MINIGL.KIT.json and
# its source tarball) from a pinned amiberry-minigl-library release, so the
# module and the firmware can be released on their own schedules.
# util/minigl-kit.pin holds "none" or "<tag> <MINIGL.ZZM sha256>".
#
#   minigl-kit-pin.sh current       print the pin ("none" or "<tag> <sha256>")
#   minigl-kit-pin.sh set none      ship releases without a MiniGL module
#   minigl-kit-pin.sh set <tag>     pin that library release; records the
#                                   SHA-256 of its MINIGL.ZZM
#   minigl-kit-pin.sh fetch <dir>   download the pinned kit into <dir> and
#                                   check MINIGL.ZZM against the pin; with
#                                   "none" it prints that and creates nothing
#
# Needs gh with access to the library repository for set <tag> and fetch.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PIN_FILE=${MINIGL_KIT_PIN_FILE:-$ROOT/util/minigl-kit.pin}
REPO=${MINIGL_KIT_REPO:-BlitterStudio/amiberry-minigl-library}

current() {
    pin=$(grep -v -e '^#' -e '^[[:space:]]*$' "$PIN_FILE" || true)
    if [ "$(printf '%s\n' "$pin" | grep -c .)" -ne 1 ]; then
        echo "$PIN_FILE: expected exactly one pin line" >&2
        exit 1
    fi
    if [ "$pin" != none ] &&
       ! printf '%s\n' "$pin" | grep -qx -e '[A-Za-z0-9._+-][A-Za-z0-9._+-]* [0-9a-f]\{64\}'; then
        echo "$PIN_FILE: '$pin' is neither 'none' nor '<tag> <sha256>'" >&2
        exit 1
    fi
    printf '%s\n' "$pin"
}

download() {
    tag=$1
    dir=$2
    mkdir -p "$dir"
    gh release download "$tag" --repo "$REPO" --dir "$dir" --clobber \
        --pattern MINIGL.ZZM --pattern MINIGL.KIT.json \
        --pattern 'MINIGL-source-*.tar.gz'
    if [ ! -f "$dir/MINIGL.ZZM" ] || [ ! -f "$dir/MINIGL.KIT.json" ]; then
        echo "$REPO release $tag has no MINIGL.ZZM and MINIGL.KIT.json" >&2
        exit 1
    fi
}

module_sha() {
    if command -v sha256sum > /dev/null 2>&1; then
        sha256sum "$1/MINIGL.ZZM" | cut -d' ' -f1
    else
        shasum -a 256 "$1/MINIGL.ZZM" | cut -d' ' -f1
    fi
}

write_pin() {
    # Rewrite in place through cat so the file keeps its mode.
    { grep -e '^#' "$PIN_FILE"; printf '%s\n' "$1"; } > "$PIN_FILE.pin-tmp"
    cat "$PIN_FILE.pin-tmp" > "$PIN_FILE"
    rm -f "$PIN_FILE.pin-tmp"
    current > /dev/null
}

case ${1:-} in
    current)
        current
        ;;
    set)
        tag=${2:-}
        if [ -z "$tag" ]; then
            echo "usage: $0 set <tag>|none" >&2
            exit 64
        fi
        if [ "$tag" = none ]; then
            write_pin none
            exit 0
        fi
        scratch=$(mktemp -d)
        trap 'rm -rf "$scratch"' EXIT
        download "$tag" "$scratch"
        write_pin "$tag $(module_sha "$scratch")"
        ;;
    fetch)
        dir=${2:-}
        if [ -z "$dir" ]; then
            echo "usage: $0 fetch <dir>" >&2
            exit 64
        fi
        pin=$(current)
        if [ "$pin" = none ]; then
            echo "[minigl-kit] none pinned; the release ships without MINIGL.ZZM"
            exit 0
        fi
        tag=${pin% *}
        expected=${pin#* }
        download "$tag" "$dir"
        actual=$(module_sha "$dir")
        if [ "$actual" != "$expected" ]; then
            echo "$REPO release $tag: MINIGL.ZZM sha256 $actual, pinned $expected" >&2
            exit 1
        fi
        echo "[minigl-kit] $tag MINIGL.ZZM $actual"
        ;;
    *)
        echo "usage: $0 current | set <tag>|none | fetch <dir>" >&2
        exit 64
        ;;
esac
