#!/bin/bash
# Check which bitstream builder build_variant_bitstreams.sh picks for every
# release variant, using stand-in builder commands on a scratch copy of the
# files the script edits. A wrong pick (e.g. the E7M RasterIX builder for the
# C28 zorro3-aga image) writes a wrong image to a release path.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/bootimage_work/variants"
cp "$ROOT/build_variant_bitstreams.sh" "$ROOT/mntzorro.v" "$WORK/"
cp "$ROOT/bootimage_work/variants/variant-matrix.sh" "$WORK/bootimage_work/variants/"
: > "$WORK/bootimage_work/zz9000_ps_wrapper.bit"

LOG="$WORK/builders.log"
cat > "$WORK/stub.sh" <<'EOF'
#!/bin/bash
# $1 = builder name, $2 = output path (absent for the canonical builder)
echo "$1 ${2:-canonical}" >> "$BUILDER_LOG"
[ -n "${2:-}" ] && : > "$2"
exit 0
EOF
chmod +x "$WORK/stub.sh"

export BUILDER_LOG="$LOG"
export BITSTREAM_BUILDER="$WORK/stub.sh standard"
export C28_BITSTREAM_BUILDER="$WORK/stub.sh c28"
export RASTERIX_BITSTREAM_BUILDER="$WORK/stub.sh rasterix"
export RASTERIX_C28_BITSTREAM_BUILDER="$WORK/stub.sh rasterix-c28"

source "$ROOT/bootimage_work/variants/variant-matrix.sh"

failures=0
while IFS='|' read -r variant bitstream _ _ _ _ _ _ _ capture_c28 elf_set; do
    case "$elf_set:$capture_c28" in
        rasterix:1) expected="rasterix-c28 $bitstream" ;;
        rasterix:0) expected="rasterix $bitstream" ;;
        :1) expected="c28 $bitstream" ;;
        *) expected="standard canonical" ;;
    esac
    : > "$LOG"
    (cd "$WORK" && ./build_variant_bitstreams.sh "$variant" > /dev/null)
    actual=$(cat "$LOG")
    if [ "$actual" != "$expected" ]; then
        echo "FAIL $variant: expected '$expected', builder log '$actual'" >&2
        failures=$((failures + 1))
    else
        echo "PASS $variant: $actual"
    fi
done < <(variant_matrix_rows)

if ! cmp -s "$ROOT/mntzorro.v" "$WORK/mntzorro.v"; then
    echo "FAIL mntzorro.v was not restored after the run" >&2
    failures=$((failures + 1))
fi

[ "$failures" -eq 0 ] || exit 1
echo "variant builder selection: all variants pass"
