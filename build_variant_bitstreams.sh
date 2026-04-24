#!/bin/bash
#
# Build FPGA bitstreams for release variants.
#
# This is meant for the Vivado machine. It rewrites only the small variant
# define block in mntzorro.v, runs build_bitstream.sh, copies the resulting
# .bit file to the release path, then restores mntzorro.v.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

MNTZORRO=mntzorro.v
CANONICAL_BIT=bootimage_work/zz9000_ps_wrapper.bit
VARIANT_DIR=bootimage_work/variants

usage() {
    cat >&2 <<'EOF'
Usage: ./build_variant_bitstreams.sh [variant ...]

Builds all release variants when no variant is specified.

Variants:
  zorro3       Zorro III / A3000 / A4000
  zorro2       Zorro II 4MB / A2000
  zorro2-2mb   Zorro II 2MB / A2000
  a500         A500 4MB / ZZ9500CX Denise adapter
  a500-2mb     A500 2MB / ZZ9500CX Denise adapter
  a500plus     A500+ or Super Denise / ZZ9500CX Denise adapter
EOF
}

all_variants=(zorro3 zorro2 zorro2-2mb a500 a500-2mb a500plus)

variant_label() {
    case "$1" in
        zorro3) echo "Zorro III / A3000 / A4000" ;;
        zorro2) echo "Zorro II 4MB / A2000" ;;
        zorro2-2mb) echo "Zorro II 2MB / A2000" ;;
        a500) echo "A500 4MB / ZZ9500CX Denise adapter" ;;
        a500-2mb) echo "A500 2MB / ZZ9500CX Denise adapter" ;;
        a500plus) echo "A500+ or Super Denise / ZZ9500CX Denise adapter" ;;
        *) return 1 ;;
    esac
}

variant_output() {
    case "$1" in
        zorro3) echo "$CANONICAL_BIT" ;;
        zorro2) echo "$VARIANT_DIR/zz9000_ps_wrapper-zorro2.bit" ;;
        zorro2-2mb) echo "$VARIANT_DIR/zz9000_ps_wrapper-zorro2-2mb.bit" ;;
        a500) echo "$VARIANT_DIR/zz9000_ps_wrapper-a500.bit" ;;
        a500-2mb) echo "$VARIANT_DIR/zz9000_ps_wrapper-a500-2mb.bit" ;;
        a500plus) echo "$VARIANT_DIR/zz9000_ps_wrapper-a500plus.bit" ;;
        *) return 1 ;;
    esac
}

variant_block() {
    case "$1" in
        zorro3)
            cat <<'EOF'
// ZORRO2/3 switch
//`define ZORRO2
`define ZORRO3

// use only together with ZORRO2:
//`define VARIANT_ZZ9500        // uses Denise adapter/A500 specific video capture
//`define VARIANT_2MB           // uses only 2MB address space
//`define VARIANT_SUPERDENISE   // for A500+ and super denise

//`define VARIANT_FW20
`define VARIANT_Z3_FASTRAM
`define VARIANT_AUTOBOOT        // enable autoboot ROM

EOF
            ;;
        zorro2)
            cat <<'EOF'
// ZORRO2/3 switch
`define ZORRO2
//`define ZORRO3

// use only together with ZORRO2:
//`define VARIANT_ZZ9500        // uses Denise adapter/A500 specific video capture
//`define VARIANT_2MB           // uses only 2MB address space
//`define VARIANT_SUPERDENISE   // for A500+ and super denise

//`define VARIANT_FW20
//`define VARIANT_Z3_FASTRAM
`define VARIANT_AUTOBOOT        // enable autoboot ROM

EOF
            ;;
        zorro2-2mb)
            cat <<'EOF'
// ZORRO2/3 switch
`define ZORRO2
//`define ZORRO3

// use only together with ZORRO2:
//`define VARIANT_ZZ9500        // uses Denise adapter/A500 specific video capture
`define VARIANT_2MB           // uses only 2MB address space
//`define VARIANT_SUPERDENISE   // for A500+ and super denise

//`define VARIANT_FW20
//`define VARIANT_Z3_FASTRAM
`define VARIANT_AUTOBOOT        // enable autoboot ROM

EOF
            ;;
        a500)
            cat <<'EOF'
// ZORRO2/3 switch
`define ZORRO2
//`define ZORRO3

// use only together with ZORRO2:
`define VARIANT_ZZ9500        // uses Denise adapter/A500 specific video capture
//`define VARIANT_2MB           // uses only 2MB address space
//`define VARIANT_SUPERDENISE   // for A500+ and super denise

//`define VARIANT_FW20
//`define VARIANT_Z3_FASTRAM
`define VARIANT_AUTOBOOT        // enable autoboot ROM

EOF
            ;;
        a500-2mb)
            cat <<'EOF'
// ZORRO2/3 switch
`define ZORRO2
//`define ZORRO3

// use only together with ZORRO2:
`define VARIANT_ZZ9500        // uses Denise adapter/A500 specific video capture
`define VARIANT_2MB           // uses only 2MB address space
//`define VARIANT_SUPERDENISE   // for A500+ and super denise

//`define VARIANT_FW20
//`define VARIANT_Z3_FASTRAM
`define VARIANT_AUTOBOOT        // enable autoboot ROM

EOF
            ;;
        a500plus)
            cat <<'EOF'
// ZORRO2/3 switch
`define ZORRO2
//`define ZORRO3

// use only together with ZORRO2:
//`define VARIANT_ZZ9500        // uses Denise adapter/A500 specific video capture
//`define VARIANT_2MB           // uses only 2MB address space
`define VARIANT_SUPERDENISE   // for A500+ and super denise

//`define VARIANT_FW20
//`define VARIANT_Z3_FASTRAM
`define VARIANT_AUTOBOOT        // enable autoboot ROM

EOF
            ;;
        *) return 1 ;;
    esac
}

replace_define_block() {
    block_file=$1
    tmp_file=$(mktemp "${TMPDIR:-/tmp}/mntzorro.XXXXXX")

    if ! awk '
        NR == FNR {
            block = block $0 ORS
            next
        }
        /^\/\/ ZORRO2\/3 switch$/ {
            printf "%s", block
            skipping = 1
            found_start = 1
            next
        }
        skipping && /^`define C_S_AXI_DATA_WIDTH 32$/ {
            skipping = 0
            found_end = 1
            print
            next
        }
        !skipping {
            print
        }
        END {
            if (!found_start || !found_end) {
                exit 42
            }
        }
    ' "$block_file" "$MNTZORRO" > "$tmp_file"; then
        rm -f "$tmp_file"
        echo "ERROR: failed to replace variant define block in $MNTZORRO" >&2
        exit 1
    fi

    mv "$tmp_file" "$MNTZORRO"
}

selected=()
if [ "$#" -eq 0 ]; then
    selected=("${all_variants[@]}")
else
    for variant in "$@"; do
        case "$variant" in
            -h|--help)
                usage
                exit 0
                ;;
        esac
        if ! variant_label "$variant" >/dev/null; then
            echo "ERROR: unknown variant: $variant" >&2
            usage
            exit 1
        fi
        selected+=("$variant")
    done
fi

if [ ! -f "$MNTZORRO" ]; then
    echo "ERROR: missing $MNTZORRO" >&2
    exit 1
fi

mkdir -p "$VARIANT_DIR"

source_backup=$(mktemp "${TMPDIR:-/tmp}/mntzorro-original.XXXXXX")
canonical_backup=$(mktemp "${TMPDIR:-/tmp}/zz9000-canonical.XXXXXX")
zorro3_result=$(mktemp "${TMPDIR:-/tmp}/zz9000-zorro3.XXXXXX")
block_tmp=$(mktemp "${TMPDIR:-/tmp}/mntzorro-block.XXXXXX")
had_canonical=0
completed=0

cp "$MNTZORRO" "$source_backup"
if [ -f "$CANONICAL_BIT" ]; then
    cp "$CANONICAL_BIT" "$canonical_backup"
    had_canonical=1
fi

cleanup() {
    if [ -f "$source_backup" ]; then
        cp "$source_backup" "$MNTZORRO"
    fi

    if [ "$completed" -eq 1 ] && [ -s "$zorro3_result" ]; then
        cp "$zorro3_result" "$CANONICAL_BIT"
    elif [ "$had_canonical" -eq 1 ]; then
        cp "$canonical_backup" "$CANONICAL_BIT"
    else
        rm -f "$CANONICAL_BIT"
    fi

    rm -f "$source_backup" "$canonical_backup" "$zorro3_result" "$block_tmp"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

for variant in "${selected[@]}"; do
    output=$(variant_output "$variant")
    label=$(variant_label "$variant")

    echo "[variant] configuring $variant: $label"
    variant_block "$variant" > "$block_tmp"
    replace_define_block "$block_tmp"

    ./build_bitstream.sh

    mkdir -p "$(dirname "$output")"
    if [ "$output" != "$CANONICAL_BIT" ]; then
        cp "$CANONICAL_BIT" "$output"
    fi
    if [ "$variant" = zorro3 ]; then
        cp "$CANONICAL_BIT" "$zorro3_result"
    fi
    ls -lh "$output"
done

completed=1
echo "[variant] done. Restored $MNTZORRO."
if [ -s "$zorro3_result" ]; then
    echo "[variant] canonical bitstream is the rebuilt zorro3 image."
else
    echo "[variant] canonical bitstream restored to its previous image."
fi
