#!/bin/bash
#
# Build release-ready firmware archives.
#
# Each archive mirrors the old public firmware ZIP shape:
#   zz9000-firmware-<tag>-<variant>/BOOT.bin
#
# CI can only package variants that already have a committed bitstream.
# The ARM firmware ELF is shared across all variants.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

usage() {
    cat >&2 <<'EOF'
Usage: ./build_release_assets.sh [--tag TAG] [--output DIR] [--require-all]
                                 [--firmware-flavor SUFFIX]

Options:
  --tag TAG               Release/build label used in archive names
                          (default: local)
  --output DIR            Directory to write release assets into
                          (default: release)
  --require-all           Fail if any known variant bitstream is missing
  --firmware-flavor SUFFIX
                          Append SUFFIX to each archive name, e.g. "ns-pal"
                          produces zz9000-firmware-<tag>-<variant>-ns-pal.zip.
                          Use this when packaging an alternate ZZ9000OS.elf
                          flavor (the script does not rebuild firmware —
                          the caller is expected to do that). Skips the
                          canonical bootimage_work/BOOT.bin overwrite so the
                          standard-flavor BOOT.bin remains the local default.
EOF
}

TAG=local
OUT_DIR=release
REQUIRE_ALL=0
FIRMWARE_FLAVOR=

while [ "$#" -gt 0 ]; do
    case "$1" in
        --tag)
            if [ "$#" -lt 2 ]; then
                echo "ERROR: --tag needs a value." >&2
                usage
                exit 1
            fi
            TAG=$2
            shift 2
            ;;
        --output)
            if [ "$#" -lt 2 ]; then
                echo "ERROR: --output needs a directory." >&2
                usage
                exit 1
            fi
            OUT_DIR=$2
            shift 2
            ;;
        --require-all)
            REQUIRE_ALL=1
            shift
            ;;
        --firmware-flavor)
            if [ "$#" -lt 2 ]; then
                echo "ERROR: --firmware-flavor needs a value." >&2
                usage
                exit 1
            fi
            FIRMWARE_FLAVOR=$2
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "ERROR: unknown argument: $1" >&2
            usage
            exit 1
            ;;
    esac
done

if ! command -v zip >/dev/null 2>&1; then
    echo "ERROR: zip not found on PATH." >&2
    exit 1
fi

if [ ! -f ZZ9000_proto.sdk/ZZ9000OS/build/ZZ9000OS.elf ]; then
    echo "ERROR: missing ZZ9000OS.elf; run ./build_firmware.sh first." >&2
    exit 1
fi

variant_defs=(
    "zorro3|bootimage_work/zz9000_ps_wrapper.bit|Zorro III, A3000/A4000"
    "zorro3-nofast|bootimage_work/variants/zz9000_ps_wrapper-zorro3-nofast.bit|Zorro III, A3000/A4000, no Zorro RAM"
    "zorro2|bootimage_work/variants/zz9000_ps_wrapper-zorro2.bit|Zorro II 4MB, A2000"
    "zorro2-2mb|bootimage_work/variants/zz9000_ps_wrapper-zorro2-2mb.bit|Zorro II 2MB, A2000"
    "a500|bootimage_work/variants/zz9000_ps_wrapper-a500.bit|A500 4MB, ZZ9500CX Denise adapter"
    "a500-2mb|bootimage_work/variants/zz9000_ps_wrapper-a500-2mb.bit|A500 2MB, ZZ9500CX Denise adapter"
    "a500plus|bootimage_work/variants/zz9000_ps_wrapper-a500plus.bit|A500+ or Super Denise, ZZ9500CX Denise adapter"
)

mkdir -p "$OUT_DIR"
# Each pass deletes only the exact archive names it's about to (re)write,
# so other-flavor passes in the same output dir aren't disturbed.
for def in "${variant_defs[@]}"; do
    IFS='|' read -r variant _ _ <<< "$def"
    if [ -n "$FIRMWARE_FLAVOR" ]; then
        rm -f "$OUT_DIR/zz9000-firmware-${TAG}-${variant}-${FIRMWARE_FLAVOR}.zip"
    else
        rm -f "$OUT_DIR/zz9000-firmware-${TAG}-${variant}.zip"
    fi
done
TMP_DIR="$OUT_DIR/.tmp"
rm -rf "$TMP_DIR"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT

created=()
missing=()

for def in "${variant_defs[@]}"; do
    IFS='|' read -r variant bitstream label <<< "$def"

    if [ ! -f "$bitstream" ]; then
        missing+=("$variant ($bitstream)")
        continue
    fi

    if [ -n "$FIRMWARE_FLAVOR" ]; then
        archive_dir="zz9000-firmware-${TAG}-${variant}-${FIRMWARE_FLAVOR}"
        flavor_label=" [${FIRMWARE_FLAVOR}]"
    else
        archive_dir="zz9000-firmware-${TAG}-${variant}"
        flavor_label=""
    fi
    archive_root="$TMP_DIR/$archive_dir"
    boot_bin="$archive_root/BOOT.bin"
    zip_path="$OUT_DIR/${archive_dir}.zip"

    echo "[release] building $variant${flavor_label}: $label"
    mkdir -p "$archive_root"
    ./build_bootimage.sh --bitstream "$bitstream" --output "$boot_bin"

    rm -f "$zip_path"
    (
        cd "$TMP_DIR"
        zip -qr "../$(basename "$zip_path")" "$archive_dir"
    )
    created+=("$zip_path")

    # Only the standard-flavor zorro3 build refreshes the canonical
    # local BOOT.bin — alternate flavors are release artifacts only.
    if [ "$variant" = zorro3 ] && [ -z "$FIRMWARE_FLAVOR" ]; then
        cp "$boot_bin" bootimage_work/BOOT.bin
    fi
done

if [ "${#missing[@]}" -gt 0 ]; then
    if [ "$REQUIRE_ALL" -eq 1 ]; then
        echo "[release] missing required variant bitstreams:" >&2
    else
        echo "[release] missing optional variant bitstreams:" >&2
    fi
    printf '  - %s\n' "${missing[@]}" >&2
    if [ "$REQUIRE_ALL" -eq 1 ]; then
        exit 1
    fi
fi

if [ "${#created[@]}" -eq 0 ]; then
    echo "ERROR: no release archives created." >&2
    exit 1
fi

echo "[release] assets:"
while IFS= read -r asset; do
    ls -lh "$asset"
done < <(find "$OUT_DIR" -maxdepth 1 -type f -print | sort)
