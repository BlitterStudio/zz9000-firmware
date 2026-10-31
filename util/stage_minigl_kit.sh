#!/bin/sh
# Stage a verified MiniGL module kit into a firmware release directory.
set -eu

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo "usage: $0 KIT_DIR DESTINATION [--require-source]" >&2
    exit 2
fi

kit_dir=$1
destination=$2
require_source=${3:-}
module=$kit_dir/MINIGL.ZZM
manifest=$kit_dir/MINIGL.KIT.json

if [ ! -f "$module" ] || [ ! -f "$manifest" ]; then
    echo "ERROR: kit must contain MINIGL.ZZM and MINIGL.KIT.json" >&2
    exit 1
fi

case "$require_source" in
    "") ;;
    --require-source) ;;
    *)
        echo "ERROR: unknown option: $require_source" >&2
        exit 2
        ;;
esac

set -- "$kit_dir"/MINIGL-source-*.tar.gz
if [ -e "$1" ] && [ "$#" -eq 1 ]; then
    source_tarball=$1
else
    source_tarball=
fi
if [ "$require_source" = --require-source ] && [ -z "$source_tarball" ]; then
    echo "ERROR: kit must contain exactly one MINIGL-source-*.tar.gz" >&2
    exit 1
fi

if [ -n "$source_tarball" ]; then
    python3 util/check_minigl_kit.py validate "$manifest" --module "$module" \
        --source-tarball "$source_tarball"
else
    python3 util/check_minigl_kit.py validate "$manifest" --module "$module"
fi
mkdir -p "$destination/ZZ9K"
install -m 0644 "$module" "$destination/MINIGL.ZZM"
install -m 0644 "$manifest" "$destination/MINIGL.KIT.json"
install -m 0644 "$module" "$destination/ZZ9K/MINIGL.ZZM"
install -m 0644 "$manifest" "$destination/ZZ9K/MINIGL.KIT.json"
if [ -n "$source_tarball" ]; then
    install -m 0644 "$source_tarball" "$destination/$(basename "$source_tarball")"
fi
