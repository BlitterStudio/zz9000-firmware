#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the scene generator in gcc:13, emit scenes plus analytic reference
# frames, replay every scene in xsim, and gate on full-image comparison.
#
# usage: run_scenes.sh [output-dir] [latency]
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd | tr '\\' '/')
IF=${HERE%/test/rix_xsim}
MG=${RIX_MINIGL_WORKTREE:-D:/Github/zz9000-minigl-worktree}
OUT=${1:-$HERE/out/scenes}
LATENCY=${2:-2}
WINIF=$(printf '%s\n' "$IF" | tr '/' '\\')
WINOUT=$(printf '%s\n' "$OUT" | tr '/' '\\')

mkdir -p "$OUT"

docker run --rm -v "$IF:/if:ro" -v "$MG:/mg:ro" -v "$OUT:/out" gcc:13 sh -ec '
  gcc -std=c99 -Wall -Wextra -Werror -I /mg/ZZ9000_proto.sdk/ZZ9000OS/src \
      /if/test/rix_xsim/rix_xsim_gen.c \
      /mg/ZZ9000_proto.sdk/ZZ9000OS/src/rix_cmd.c \
      /mg/ZZ9000_proto.sdk/ZZ9000OS/src/rix_setup.c \
      /mg/ZZ9000_proto.sdk/ZZ9000OS/src/rix_bin.c \
      /mg/ZZ9000_proto.sdk/ZZ9000OS/src/rix_texture.c \
      -lm -o /tmp/rix_xsim_gen
  /tmp/rix_xsim_gen /out
'

for scene in probe s1 s2 s3 s4; do
  echo "=== xsim $scene (latency $LATENCY) ==="
  rm -f "$OUT/$scene.ppm"
  cmd //c "$WINIF\\test\\rix_xsim\\run_xsim.bat" "$WINIF" "$WINOUT" "$scene" "$LATENCY" >"$OUT/${scene}_xsim.log" 2>&1
  test -f "$OUT/$scene.ppm"
  py -3 "$HERE/compare_frames.py" "$OUT/${scene}_ref.ppm" "$OUT/${scene}.ppm" \
      "$OUT/${scene}_diff.png" "$OUT/${scene}_comparison.json"
  py -3 - "$OUT/${scene}_comparison.json" "$scene" <<'EOF'
import json, sys
report = json.load(open(sys.argv[1]))
metrics = report["rgb565_lsb"]
print(f"{sys.argv[2]}: pass={report['pass']} "
      f"max={max(metrics['max'].values())} mean={max(metrics['mean'].values()):.4f} "
      f"beyond1lsb={metrics['pixels_beyond_1_lsb']} ({metrics['pixels_beyond_1_lsb_percent']:.4f}%) "
      f"coverage={metrics['coverage_mismatches']} ({metrics['coverage_mismatches_percent']:.4f}%)")
sys.exit(0 if report["pass"] else 1)
EOF
done
echo "all scenes match their references"
