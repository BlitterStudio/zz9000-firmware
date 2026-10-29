#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the scene generator in gcc:13, emit scenes plus analytic reference
# frames, replay every scene in xsim, and gate on full-image comparison.
#
# usage: run_scenes.sh [output-dir] [latency]
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd | tr '\\' '/')
IF=${HERE%/test/rix_xsim}
MG=${RIX_MINIGL_WORKTREE:-$IF}
OUT=${1:-$HERE/out/scenes}
LATENCY=${2:-2}
if command -v cygpath >/dev/null 2>&1; then
  IF_DOCKER=$(cygpath -m "$IF")
  MG_DOCKER=$(cygpath -m "$MG")
  OUT_DOCKER=$(cygpath -m "$OUT")
  HERE_WIN=$(cygpath -m "$HERE")
  OUT_WIN=$(cygpath -m "$OUT")
  WINIF=$(cygpath -w "$IF")
  WINOUT=$(cygpath -w "$OUT")
else
  IF_DOCKER=$IF
  MG_DOCKER=$MG
  OUT_DOCKER=$OUT
  HERE_WIN=$HERE
  OUT_WIN=$OUT
  WINIF=$(printf '%s\n' "$IF" | tr '/' '\\')
  WINOUT=$(printf '%s\n' "$OUT" | tr '/' '\\')
fi
export MSYS_NO_PATHCONV=1

mkdir -p "$OUT"

docker run --rm -v "$IF_DOCKER:/if:ro" -v "$MG_DOCKER:/mg:ro" -v "$OUT_DOCKER:/out" gcc:13 sh -ec '
  gcc -std=c99 -Wall -Wextra -Werror -I /mg/ZZ9000_proto.sdk/ZZ9000OS/src \
      /if/test/rix_xsim/rix_xsim_gen.c \
      /mg/ZZ9000_proto.sdk/ZZ9000OS/src/rix_cmd.c \
      /mg/ZZ9000_proto.sdk/ZZ9000OS/src/rix_setup.c \
      /mg/ZZ9000_proto.sdk/ZZ9000OS/src/rix_texture.c \
      -lm -o /tmp/rix_xsim_gen
  /tmp/rix_xsim_gen /out
'

compare_scene() {
  scene=$1
  actual=$2
  label=$3
  py -3 "$HERE_WIN/compare_frames.py" "$OUT_WIN/${scene}_ref.ppm" "$actual" \
      "$OUT_WIN/${scene}_${label}_diff.png" "$OUT_WIN/${scene}_${label}_comparison.json"
  py -3 - "$OUT_WIN/${scene}_${label}_comparison.json" "$scene/$label" <<'EOF'
import json, sys
report = json.load(open(sys.argv[1]))
metrics = report["rgb565_lsb"]
print(f"{sys.argv[2]}: pass={report['pass']} "
      f"max={max(metrics['max'].values())} mean={max(metrics['mean'].values()):.4f} "
      f"beyond1lsb={metrics['pixels_beyond_1_lsb']} ({metrics['pixels_beyond_1_lsb_percent']:.4f}%) "
      f"coverage={metrics['coverage_mismatches']} ({metrics['coverage_mismatches_percent']:.4f}%)")
sys.exit(0 if report["pass"] else 1)
EOF
}

run_scene() {
  scene=$1
  out_dir=$2
  log_tag=$3
  echo "=== xsim $scene ($log_tag, latency $LATENCY) ==="
  rm -f "$out_dir/$scene.ppm"
  cmd.exe /c "$WINIF\\test\\rix_xsim\\run_xsim.bat" "$WINIF" "$(cygpath -m "$out_dir" 2>/dev/null || printf '%s' "$out_dir")" "$scene" "$LATENCY" >"$out_dir/${scene}_${log_tag}.log" 2>&1
  test -f "$out_dir/$scene.ppm"
}

for scene in ef-s1 ef-s2 ef-s3 ef-s4 ef-probe ef-lm; do
  run_scene "$scene" "$OUT" mmio
  compare_scene "$scene" "$OUT_WIN/$scene.ppm" mmio
done


# Transport coverage beyond plain MMIO-paced submission:
#  - SHARED=1 routes both read masters through the single in-order read
#    channel model with head-of-line blocking, exercising the XACP read-credit
#    fix while texture fetches race fragment traffic (textured + stress +
#    lightmap scenes).
#  - DMA_BURST labels the same committed packet boundaries as a PL330
#    fixed-destination producer; the frame must stay byte-identical to the
#    MMIO submission.
mkdir -p "$OUT/shared" "$OUT/dma"
cp "$OUT/ef-s3.hex" "$OUT/ef-s4.hex" "$OUT/ef-lm.hex" "$OUT/textures.hex" "$OUT/shared/"
cp "$OUT/ef-s3.hex" "$OUT/textures.hex" "$OUT/dma/"
for scene in ef-s3 ef-s4 ef-lm; do
  SHARED=1 run_scene "$scene" "$OUT/shared" shared
  compare_scene "$scene" "$OUT_WIN/shared/$scene.ppm" shared
done
DMA_BURST=1 run_scene ef-s3 "$OUT/dma" dma
cmp "$OUT/ef-s3.ppm" "$OUT/dma/ef-s3.ppm"
echo "dma-fixed transport reproduces the mmio frame bit-exactly"
echo "all scenes match their references"
