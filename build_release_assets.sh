#!/bin/bash
#
# Build release-ready firmware archives.
#
# Each archive mirrors the public firmware ZIP shape:
#   zz9000-firmware-<tag>-<variant>/BOOT.bin
#   zz9000-firmware-<tag>-<variant>/ZZ9000.CFG
#   zz9000-firmware-<tag>-<variant>/ZZ9K/*.ZZM
# CI can only package variants that already have a committed bitstream.
# All seven committed release bitstreams contain the current hardware ABI,
# including the native overlay and ZZ9000AX capture paths, so every archive
# uses the same current firmware ELF. The separately built legacy-bitstream
# ELF remains available for manual compatibility packages that retain an older
# bitstream.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"
source bootimage_work/variants/variant-matrix.sh
QUALIFICATION_RECORD=${ZZ9000_QUALIFICATION_RECORD:-bootimage_work/variants/timing-qualification.json}

usage() {
    cat >&2 <<'EOF'
Usage: ./build_release_assets.sh [--tag TAG] [--output DIR] [--require-all]
                                 [--firmware-flavor SUFFIX]

Options:
  --tag TAG               Release/build label used in archive names
                          (default: local)
  --output DIR            Directory to write release assets into
                          (default: release)
  --require-all           Fail unless every release bitstream has a matching
                          passing timing qualification record
  --check-qualification   Validate all qualification records and exit
  --firmware-flavor SUFFIX
                          Append SUFFIX to each archive name, e.g. "diag"
                          produces zz9000-firmware-<tag>-<variant>-diag.zip.
                          Use this when packaging an alternate ZZ9000OS.elf
                          flavor (the script does not rebuild firmware —
                          the caller is expected to do that). Skips the
                          canonical bootimage_work/BOOT.bin overwrite so the
                          standard-flavor BOOT.bin remains the local default.
                          CI should not use this for PAL/native-video
                          defaults; use ZZ9000.CFG instead.
EOF
}

TAG=local
OUT_DIR=release
REQUIRE_ALL=0
CHECK_QUALIFICATION=0
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
        --check-qualification)
            CHECK_QUALIFICATION=1
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

validate_qualifications() {
    local args=("$QUALIFICATION_RECORD") variant bitstream label
    while IFS='|' read -r variant bitstream label _; do
        args+=("$variant" "$bitstream")
    done < <(variant_matrix_rows)

    python3 - "${args[@]}" <<'PY'
import hashlib
import json
from pathlib import Path
import sys

record_path = Path(sys.argv[1])
try:
    record = json.loads(record_path.read_text(encoding="utf-8"))
except (OSError, json.JSONDecodeError) as error:
    raise SystemExit(f"[release] invalid qualification record {record_path}: {error}")

if record.get("schema_version") != 1 or not isinstance(record.get("variants"), dict):
    raise SystemExit(f"[release] invalid qualification record schema: {record_path}")

errors = []
for index in range(2, len(sys.argv), 2):
    variant, bitstream_path = sys.argv[index:index + 2]
    bitstream = Path(bitstream_path)
    entry = record["variants"].get(variant)
    if not bitstream.is_file():
        errors.append(f"{variant}: missing bitstream {bitstream_path}")
        continue
    if not isinstance(entry, dict):
        errors.append(f"{variant}: missing qualification record")
        continue
    required = (
        "bitstream", "sha256", "source_revision", "build_id",
        "placement_directive", "pixel_slack_ns", "overall_slack_ns",
        "i2s_slack_ns", "sii9022_slack_ns", "gate_passed",
    )
    missing = [field for field in required if field not in entry]
    if missing:
        errors.append(f"{variant}: qualification record missing {', '.join(missing)}")
        continue
    if entry["bitstream"] != bitstream_path:
        errors.append(f"{variant}: qualification bitstream path does not match release matrix")
        continue
    if entry["gate_passed"] is not True:
        errors.append(f"{variant}: qualification gate did not pass")
        continue
    actual = hashlib.sha256(bitstream.read_bytes()).hexdigest()
    if entry["sha256"] != actual:
        errors.append(f"{variant}: bitstream SHA256 does not match passing qualification")

if errors:
    print("[release] qualification failures:", file=sys.stderr)
    for error in errors:
        print(f"  - {error}", file=sys.stderr)
    raise SystemExit(1)
PY
}

if [ "$REQUIRE_ALL" -eq 1 ] || [ "$CHECK_QUALIFICATION" -eq 1 ]; then
    validate_qualifications
fi
if [ "$CHECK_QUALIFICATION" -eq 1 ]; then
    echo "[release] all release bitstreams match passing timing qualifications."
    exit 0
fi

if ! command -v zip >/dev/null 2>&1; then
    echo "ERROR: zip not found on PATH." >&2
    exit 1
fi

if [ ! -f ZZ9000_proto.sdk/ZZ9000OS/build/ZZ9000OS.elf ]; then
    echo "ERROR: missing ZZ9000OS.elf; run ./build_firmware.sh first." >&2
    exit 1
fi

bash ./util/check_elf_vectors.sh ZZ9000_proto.sdk/ZZ9000OS/build/ZZ9000OS.elf

MODULE_DIR=ZZ9000_proto.sdk/ZZ9000OS/build/modules
make -C ZZ9000_proto.sdk/ZZ9000OS modules
for spec in "MPEG2 1 MPEG2 0x09000000 0x00400000" \
            "WEBM 2 WEBM 0x09400000 0x00800000" \
            "WEBP 3 WEBP 0x09c00000 0x00400000"; do
    set -- $spec
    READELF=arm-none-eabi-readelf bash util/check_zzm_conformance.sh \
        "$MODULE_DIR/$(printf '%s' "$1" | tr A-Z a-z)/$1.elf" "$2" "$3" "$4" "$5"
done
for module in MPEG2.ZZM WEBM.ZZM WEBP.ZZM; do
    if [ ! -f "$MODULE_DIR/$module" ]; then
        echo "ERROR: missing built module $MODULE_DIR/$module." >&2
        exit 1
    fi
done

# Keep the modules alongside the archives for CI consumers that install a
# firmware artifact directly rather than unpacking a release ZIP.
mkdir -p "$OUT_DIR/ZZ9K"
cp "$MODULE_DIR"/*.ZZM "$OUT_DIR/ZZ9K/"

variant_defs=("${VARIANT_MATRIX[@]}")

mkdir -p "$OUT_DIR"
# Each pass deletes only the exact archive names it's about to (re)write,
# so other-flavor passes in the same output dir aren't disturbed.
for def in "${variant_defs[@]}"; do
    IFS='|' read -r variant _ _ _ <<< "$def"
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
    IFS='|' read -r variant bitstream label _ <<< "$def"

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
    ./build_bootimage.sh --bitstream "$bitstream" --firmware ZZ9000_proto.sdk/ZZ9000OS/build/ZZ9000OS.elf \
        --output "$boot_bin"
    # Ship the (all-commented) sample config next to BOOT.bin so users
    # discover it; copying it to the SD card as-is changes nothing.
    cp ZZ9000.CFG "$archive_root/ZZ9000.CFG"
    mkdir -p "$archive_root/ZZ9K"
    cp "$MODULE_DIR"/*.ZZM "$archive_root/ZZ9K/"

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
