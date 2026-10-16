#!/usr/bin/env bash
# Run from Git Bash: test/rix_xsim/run_xsim.sh [s1|s2|s3|s4] [latency]
set -euo pipefail

scene=${1:-s1}
latency=${2:-2}
case "$scene" in s1|s2|s3|s4) ;; *) echo "unknown scene: $scene" >&2; exit 2 ;; esac
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$script_dir/../.." && pwd)
out="$script_dir/out/$scene-latency$latency"
mkdir -p "$out"
if command -v cygpath >/dev/null 2>&1; then
  script_win=$(cygpath -aw "$script_dir")
  root_win=$(cygpath -aw "$root")
  out_win=$(cygpath -aw "$out")
else
  script_win=$script_dir
  root_win=$root
  out_win=$out
fi
minigl_root=${MINIGL_ROOT:-D:/Github/zz9000-minigl-worktree/ZZ9000_proto.sdk/ZZ9000OS/src}
if command -v cygpath >/dev/null 2>&1; then
  minigl_win=$(cygpath -aw "$minigl_root")
else
  minigl_win=$minigl_root
fi
export MSYS_NO_PATHCONV=1
docker run --rm --entrypoint /bin/bash -v "$root_win:/repo:ro" -v "$minigl_win:/minigl:ro" -v "$out_win:/out" gcc:13 -lc '
  set -euo pipefail
  gcc -std=c99 -O2 -Wall -Wextra -Werror -I/minigl \
      /repo/test/rix_xsim/rix_xsim_gen.c /minigl/rix_cmd.c /minigl/rix_setup.c \
      /minigl/rix_bin.c /minigl/rix_texture.c -lm -o /out/rix_xsim_gen
  /out/rix_xsim_gen /out
'
cmd.exe /c "call $script_win\\run_xsim.bat $root_win $out_win $scene $latency"
