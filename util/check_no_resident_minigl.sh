#!/bin/sh
# Reject a firmware link that retains MiniGL code or the private front-end path.
set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 FIRMWARE_ELF LINK_MAP MAKEFILE" >&2
    exit 2
fi

elf=$1
link_map=$2
makefile=$3

for path in "$elf" "$link_map" "$makefile"; do
    if [ ! -f "$path" ]; then
        echo "ERROR: resident MiniGL gate needs $path" >&2
        exit 2
    fi
done

if grep -E -q 'MINIGL_LIB_DIR|MGL_FRONT_(SRCS|OBJS)' "$makefile"; then
    echo "ERROR: firmware Makefile still declares the private MiniGL front end." >&2
    exit 1
fi

if grep -E -q '(^|/)(mgl/|mw_[^/ ]*\.o|minigl_[^/ ]*\.o|rix_[^/ ]*\.o|zzapp_gl\.o)' "$link_map"; then
    echo "ERROR: ZZ9000OS.elf still links a resident MiniGL object." >&2
    exit 1
fi

# The ELF argument makes this gate fail early for a stale/missing link output;
# the map supplies the object-level proof that an ELF symbol table cannot.
if [ ! -s "$elf" ]; then
    echo "ERROR: firmware ELF is empty." >&2
    exit 1
fi
