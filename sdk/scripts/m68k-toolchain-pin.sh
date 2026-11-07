#!/bin/sh
# Copyright (C) 2026, Dimitris Panokostas / BlitterStudio
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The m68k compiler image is pinned by digest wherever it is named
# (amigadev/crosstools:<tag>@sha256:<digest>): the tag follows GCC 16 release
# candidates. This script finds the pins in the tracked files.
#
#   m68k-toolchain-pin.sh current       print the pinned image; fails if two
#                                       pins differ
#   m68k-toolchain-pin.sh set <image>   rewrite every pin to <image>, which
#                                       must be <repo>:<tag>@sha256:<digest>
#
# .github/workflows/m68k-toolchain.yml uses it to follow the tag.
set -eu

pattern='amigadev/crosstools:m68k-amigaos-[A-Za-z0-9._-]*@sha256:[0-9a-f]\{64\}'
cd "$(git rev-parse --show-toplevel)"

current() {
    pins=$(git grep -h -o -e "$pattern" | sort -u)
    count=$(printf '%s\n' "$pins" | grep -c . || true)
    if [ "$count" -ne 1 ]; then
        echo "expected one pinned m68k image, found $count:" >&2
        printf '%s\n' "$pins" >&2
        exit 1
    fi
    printf '%s\n' "$pins"
}

case ${1:-} in
    current)
        current
        ;;
    set)
        new=${2:-}
        if ! printf '%s\n' "$new" | grep -qx -e "$pattern"; then
            echo "not a pinned image reference: '$new'" >&2
            exit 64
        fi
        old=$(current)
        [ "$old" = "$new" ] && exit 0
        git grep -l -F -e "$old" | while IFS= read -r file; do
            # Rewrite in place through cat so the file keeps its mode.
            sed "s|$old|$new|g" "$file" > "$file.pin-tmp"
            cat "$file.pin-tmp" > "$file"
            rm -f "$file.pin-tmp"
        done
        current > /dev/null
        ;;
    *)
        echo "usage: $0 current | set <image@sha256:digest>" >&2
        exit 64
        ;;
esac
