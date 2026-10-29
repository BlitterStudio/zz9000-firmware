#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the scene generator in gcc:13, emit scenes plus analytic reference
# frames, replay every scene in xsim, and gate on full-image comparison.
#
# usage: run_scenes.sh [output-dir] [latency] [matrix]
#   matrix: all (default), stream (the stream-level transports) or fifo (the
#   real GP1 -> ps7_0_axi_periph -> axi_fifo_mm_s ingress, fifo_path/).
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd | tr '\\' '/')
IF=${HERE%/test/rix_xsim}
MG=${RIX_MINIGL_WORKTREE:-$IF}
OUT=${1:-$HERE/out/scenes}
LATENCY=${2:-2}
MATRIX=${3:-all}
case "$MATRIX" in
  all|stream|fifo) ;;
  *) echo "matrix must be all, stream or fifo" >&2; exit 2 ;;
esac
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
      /mg/ZZ9000_proto.sdk/ZZ9000OS/src/rix_drv.c \
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

# run_scene SCENE OUT_DIR LOG_TAG [VAR=value ...]: the VAR=value pairs become
# run_xsim.bat environment (SHARED, DMA_BURST, RAND_SEED, ...).
run_scene() {
  scene=$1
  out_dir=$2
  log_tag=$3
  shift 3
  envs=""
  for kv in "$@"; do envs="${envs}set ${kv}&& "; done
  echo "=== xsim $scene ($log_tag, latency $LATENCY) ==="
  rm -f "$out_dir/$scene.ppm"
  out_win=$(cygpath -m "$out_dir" 2>/dev/null || printf '%s' "$out_dir")
  # One command string so the leading `set` clauses parse; paths contain no spaces.
  cmd.exe /c "${envs}$WINIF\\test\\rix_xsim\\run_xsim.bat $WINIF $out_win $scene $LATENCY" >"$out_dir/${scene}_${log_tag}.log" 2>&1
  grep -E "^RESULT" "$out_dir/${scene}_${log_tag}.log" || true
  test -f "$out_dir/$scene.ppm"
}

# run_fifo_scene SCENE TRANSPORT: replay SCENE.fifo through the generated
# GP1 interconnect and axi_fifo_mm_s (fifo_path/run_fifo_path.bat).  The
# testbench checks every streamed word and TLAST against the committed
# packets and fails on a rejected TLF; pl330-early instead must end with
# ISR.TSE set and nothing streamed.
run_fifo_scene() {
  scene=$1
  transport=$2
  log="$OUT/${scene}_fifo-$transport.log"
  echo "=== xsim $scene (fifo-$transport, latency $LATENCY) ==="
  rm -f "$OUT/${scene}_fifo-$transport.ppm"
  status=0
  cmd.exe /c "$WINIF\\test\\rix_xsim\\fifo_path\\run_fifo_path.bat run $FIFO_WORK_WIN $WINOUT $scene $LATENCY $transport" >"$log" 2>&1 || status=$?
  grep -E "^(RESULT|FIFO_PATH|PASS)" "$log" || true
  if [ "$transport" = pl330-early ]; then
    expected="^PASS scene=$scene early TLF rejected"
  else
    expected="^FIFO_PATH packets=[0-9]+ words=[0-9]+ streamed exactly"
  fi
  if [ "$status" -ne 0 ] || ! grep -Eq "$expected" "$log" || ! grep -Eq "^PASS scene=$scene" "$log" ||
     { [ "$transport" != pl330-early ] && [ ! -f "$OUT/${scene}_fifo-$transport.ppm" ]; }; then
    grep -E "Fatal|FATAL|ERROR" "$log" >&2 || true
    echo "$scene fifo-$transport failed (exit $status), see $log" >&2
    exit 1
  fi
}

SCENES="ef-s1 ef-s2 ef-s3 ef-s4 ef-probe ef-lm ef-stress"
if [ "$MATRIX" != fifo ]; then
  for scene in $SCENES; do
    run_scene "$scene" "$OUT" mmio
    compare_scene "$scene" "$OUT_WIN/$scene.ppm" mmio
  done

  # Transport coverage beyond plain MMIO-paced submission:
  #  - SHARED=1 routes every renderer read through one in-order response
  #    stream with head-of-line blocking (the HP-port hazard the per-port
  #    response reservations in RasterIX_EF.v remove); ef-stress additionally
  #    runs with randomized per-beat latency and AXI-legal cross-ID reordering.
  #  - DMA_BURST labels the same committed packet boundaries as a PL330
  #    fixed-destination producer; the frame must stay byte-identical to the
  #    MMIO submission.
  mkdir -p "$OUT/shared" "$OUT/dma" "$OUT/random"
  for scene in $SCENES; do
    cp "$OUT/$scene.hex" "$OUT/shared/"
    cp "$OUT/$scene.hex" "$OUT/dma/"
  done
  cp "$OUT/textures.hex" "$OUT/shared/"
  cp "$OUT/textures.hex" "$OUT/dma/"
  cp "$OUT/ef-stress.hex" "$OUT/textures.hex" "$OUT/random/"
  for scene in $SCENES; do
    run_scene "$scene" "$OUT/shared" shared SHARED=1
    compare_scene "$scene" "$OUT_WIN/shared/$scene.ppm" shared
    run_scene "$scene" "$OUT/dma" dma DMA_BURST=1
    compare_scene "$scene" "$OUT_WIN/dma/$scene.ppm" dma
    cmp "$OUT/$scene.ppm" "$OUT/dma/$scene.ppm"
  done
  for seed in 1 2 3; do
    run_scene ef-stress "$OUT/random" "seed$seed" SHARED=1 RAND_SEED=$seed
    mv "$OUT/random/ef-stress.ppm" "$OUT/random/ef-stress-seed$seed.ppm"
    compare_scene ef-stress "$OUT_WIN/random/ef-stress-seed$seed.ppm" "seed$seed"
  done
  echo "dma-fixed transport reproduces every mmio frame bit-exactly"
fi

# Real command ingress: GP1 (AXI3) -> ps7_0_axi_periph (AXI3 -> AXI4-Lite,
# crossbar, clock converters) -> gpu_cmd_fifo (axi_fifo_mm_s) -> RasterIX EF,
# generated from zz9000_project.tcl, replaying the packets rix_drv.c commits
# over MMIO stores, the firmware's PL330 program (single-beat FIXED stores)
# and 16-beat FIXED bursts.  Every frame must equal the fifo-mmio frame and,
# in the full matrix, the stream-level mmio frame bit for bit.
if [ "$MATRIX" != stream ]; then
  FIFO_WORK=$OUT/fifo_path_work
  FIFO_WORK_WIN=$(cygpath -w "$FIFO_WORK" 2>/dev/null || printf '%s\n' "$FIFO_WORK" | tr '/' '\\')
  echo "=== build fifo_path ==="
  cmd.exe /c "$WINIF\\test\\rix_xsim\\fifo_path\\run_fifo_path.bat build $WINIF $FIFO_WORK_WIN" >"$OUT/fifo_path_build.log" 2>&1 || {
    grep -E "FIFO_PATH_ERROR|^ERROR" "$OUT/fifo_path_build.log" >&2 || true
    echo "fifo_path build failed, see $OUT/fifo_path_build.log" >&2
    exit 1
  }
  grep -E "^FIFO_PATH_TOPOLOGY" "$OUT/fifo_path_build.log"
  for scene in $SCENES; do
    for transport in mmio pl330 burst16; do
      run_fifo_scene "$scene" "$transport"
      compare_scene "$scene" "$OUT_WIN/${scene}_fifo-$transport.ppm" "fifo-$transport"
      cmp "$OUT/${scene}_fifo-mmio.ppm" "$OUT/${scene}_fifo-$transport.ppm"
    done
    if [ "$MATRIX" = all ]; then
      cmp "$OUT/$scene.ppm" "$OUT/${scene}_fifo-mmio.ppm"
    fi
    run_fifo_scene "$scene" pl330-early
  done
  echo "fifo path delivers every committed packet over mmio, pl330 and burst16 bit-exactly"
  echo "fifo path rejects the early-TLF race with ISR.TSE and streams nothing"
fi
echo "all scenes match their references"
