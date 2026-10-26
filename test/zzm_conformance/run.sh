#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

if [ "$#" -ne 5 ]; then
    echo "usage: $0 CHECK READELF MPEG2_ELF WEBM_ELF WEBP_ELF" >&2
    exit 2
fi

check=$1
readelf=$2
mpeg2=$3
webm=$4
webp=$5
fixture_dir=$(CDPATH= cd -- "$(dirname -- "$0")/build" && pwd)

conform() {
    READELF="$readelf" "$check" "$1" "$2" "$3" "$4" "$5"
}

expect_pass() {
    conform "$@"
}

expect_failure() {
    name=$1
    expected=$2
    elf=$3
    if output=$(conform "$elf" 1 fixture 0x09000000 0x00400000 2>&1); then
        echo "$name unexpectedly conformed" >&2
        exit 1
    fi
    lines=$(printf '%s\n' "$output" | awk 'END { print NR }')
    if [ "$lines" -ne 1 ]; then
        echo "$name produced $lines diagnostics, expected one:" >&2
        printf '%s\n' "$output" >&2
        exit 1
    fi
    case $output in
        *"$expected"*) ;;
        *)
            echo "$name diagnostic did not contain: $expected" >&2
            printf '%s\n' "$output" >&2
            exit 1
            ;;
    esac
}

expect_pass "$fixture_dir/valid.elf" 1 fixture 0x09000000 0x00400000
expect_failure local-heap "module-local newlib heap symbol: _sbrk" "$fixture_dir/local_heap.elf"
expect_failure stub-syscall "module-local syscall stub is not supplied by libzzm_rt: _open" "$fixture_dir/stub_syscall.elf"
expect_failure unresolved-allocator "allocator/runtime symbol is not resolved from libzzm_rt: _malloc_r" "$fixture_dir/unresolved_allocator.elf"
expect_failure undefined-symbol "undefined ELF symbol: fixture_undefined_symbol" "$fixture_dir/undefined_symbol.elf"
expect_failure throw "unwinding/throw support is not allowed: __cxa_throw" "$fixture_dir/unwind_throw.elf"
expect_failure outside-slot "allocated section outside module slot: .outside" "$fixture_dir/outside_slot.elf"
expect_failure bad-header "cannot read ELF header" "$fixture_dir/bad_header.elf"

# These are the release module ELFs produced by the build job, not fixtures.
expect_pass "$mpeg2" 1 MPEG2 0x09000000 0x00400000
expect_pass "$webm" 2 WEBM 0x09400000 0x00800000
expect_pass "$webp" 3 WEBP 0x09c00000 0x00400000
