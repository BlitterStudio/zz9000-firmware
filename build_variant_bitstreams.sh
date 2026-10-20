#!/bin/bash
#
# Build FPGA bitstreams for release variants.
#
# This is meant for the Vivado machine. It rewrites only the small variant
# define block in mntzorro.v, runs the per-variant bitstream build, copies the
# resulting .bit file to the release path, then restores mntzorro.v.
#
# The build step defaults to ./build_bitstream.sh (Linux Vivado). On a Windows
# Vivado machine, drive the PowerShell builder from Git Bash instead:
#
#   BITSTREAM_BUILDER="powershell -NoProfile -ExecutionPolicy Bypass \
#       -File ./build_bitstream.ps1" ./build_variant_bitstreams.sh zorro2
#
# Everything else — the mntzorro.v define rewriting, the canonical-bitstream
# save/restore, and the cleanup trap — is shared by both hosts.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Word-split deliberately: the Windows builder is a multi-word command.
# Build the array without `read` so the script does not depend on a
# working stdin handle.
if [ -n "${BITSTREAM_BUILDER:-}" ]; then
    # shellcheck disable=SC2206  # deliberate word split of $BITSTREAM_BUILDER
    BITSTREAM_BUILDER_CMD=(${BITSTREAM_BUILDER})
else
    BITSTREAM_BUILDER_CMD=(./build_bitstream.sh)
fi

MNTZORRO=mntzorro.v
CANONICAL_BIT=bootimage_work/zz9000_ps_wrapper.bit
VARIANT_DIR=bootimage_work/variants
source "$VARIANT_DIR/variant-matrix.sh"

usage() {
    cat >&2 <<'EOF'
Usage: ./build_variant_bitstreams.sh [variant ...]

Builds all release variants when no variant is specified.

Variants:
  zorro3          Zorro III / A3000 / A4000
  zorro3-aga      A4000/A4000T AGA video-slot capture, Zorro III Fast RAM
  zorro2          Zorro II 4MB / A2000
  zorro2-2mb      Zorro II 2MB / A2000
  a500            A500 4MB / ZZ9500CX Denise adapter
  a500-2mb        A500 2MB / ZZ9500CX Denise adapter
  a500plus        A500+ or Super Denise / ZZ9500CX Denise adapter
EOF
}

all_variants=()
for row in "${VARIANT_MATRIX[@]}"; do
    all_variants+=("${row%%|*}")
done

variant_label() {
    local row variant bitstream label
    row=$(variant_matrix_row "$1") || return 1
    IFS='|' read -r variant bitstream label _ <<< "$row"
    printf '%s\n' "$label"
}

variant_output() {
    local row variant bitstream
    row=$(variant_matrix_row "$1") || return 1
    IFS='|' read -r variant bitstream _ <<< "$row"
    printf '%s\n' "$bitstream"
}

variant_capture_c28() {
    local row variant bitstream label zorro2 zorro3 zz9500 two_mb superdenise z3_fastram capture_c28
    row=$(variant_matrix_row "$1") || return 1
    IFS='|' read -r variant bitstream label zorro2 zorro3 zz9500 two_mb superdenise z3_fastram capture_c28 <<< "$row"
    printf '%s\n' "$capture_c28"
}

write_define() {
    local name=$1 enabled=$2 suffix=${3:-}
    if [ "$enabled" = 1 ]; then
        printf '`define %s%s\n' "$name" "$suffix"
    else
        printf '//`define %s%s\n' "$name" "$suffix"
    fi
}

variant_block() {
    local row variant bitstream label zorro2 zorro3 zz9500 two_mb superdenise z3_fastram capture_c28
    row=$(variant_matrix_row "$1") || return 1
    IFS='|' read -r variant bitstream label zorro2 zorro3 zz9500 two_mb superdenise z3_fastram capture_c28 <<< "$row"

    cat <<'EOF'
// ZORRO2/3 switch
EOF
    write_define ZORRO2 "$zorro2"
    write_define ZORRO3 "$zorro3"
    cat <<'EOF'

// use only together with ZORRO2:
EOF
    write_define VARIANT_ZZ9500 "$zz9500" '        // uses Denise adapter/A500 specific video capture'
    write_define VARIANT_2MB "$two_mb" '           // uses only 2MB address space'
    write_define VARIANT_SUPERDENISE "$superdenise" '   // for A500+ and super denise'
    printf '\n//`define VARIANT_FW20\n'
    write_define VARIANT_Z3_FASTRAM "$z3_fastram"
    printf '`define VARIANT_AUTOBOOT        // enable autoboot ROM\n\n'
}

replace_define_block() {
    block_file=$1
    tmp_file=$(mktemp "${TMPDIR:-/tmp}/mntzorro.XXXXXX")

    if ! awk '
        # Windows checkouts keep CRLF endings; strip the CR so the
        # LF-anchored markers below match either line-ending style.
        /\r$/ { sub(/\r$/, "") }
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
if [ -n "${C28_BITSTREAM_BUILDER:-}" ]; then
    # shellcheck disable=SC2206  # deliberate word split of the builder command
    C28_BUILDER_CMD=(${C28_BITSTREAM_BUILDER})
elif [ -n "${BITSTREAM_BUILDER:-}" ]; then
    C28_BUILDER_CMD=()
else
    C28_BUILDER_CMD=(./build_bitstream.sh --capture-c28 --output)
fi
for variant in "${selected[@]}"; do
    if [ "$(variant_capture_c28 "$variant")" = 1 ] && [ "${#C28_BUILDER_CMD[@]}" -eq 0 ]; then
        echo "ERROR: set C28_BITSTREAM_BUILDER to a C28 builder ending in its output-path option." >&2
        exit 1
    fi
done


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

    if [ "$(variant_capture_c28 "$variant")" = 1 ]; then
        mkdir -p "$(dirname "$output")"
        "${C28_BUILDER_CMD[@]}" "$output"
    else
        "${BITSTREAM_BUILDER_CMD[@]}"
        mkdir -p "$(dirname "$output")"
        if [ "$output" != "$CANONICAL_BIT" ]; then
            cp "$CANONICAL_BIT" "$output"
        fi
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
