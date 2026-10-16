#!/usr/bin/env bash
# Run from Git Bash on Windows: test/rasterix/run.sh [output-directory]
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
OUTPUT=${1:-"$SCRIPT_DIR/out"}
OUTPUT=$(mkdir -p "$OUTPUT" && cd -- "$OUTPUT" && pwd)

# Docker Desktop's Windows client requires drive-letter host paths. MSYS path
# conversion is disabled so Docker receives them unchanged.
ROOT_DOCKER=$(cygpath -aw "$ROOT")
OUTPUT_DOCKER=$(cygpath -aw "$OUTPUT")
export MSYS_NO_PATHCONV=1
docker run --rm \
    -v "$ROOT_DOCKER:/src:ro" \
    -v "$OUTPUT_DOCKER:/out" \
    -v zz9000-rasterix-harness-cache:/cache \
    ubuntu:24.04 \
    bash -lc '
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq build-essential cmake git python3 verilator > /dev/null
printf "compiler:\n"; c++ --version
printf "cmake:\n"; cmake --version
printf "verilator: "; verilator --version

work=/cache/RasterIX
if test ! -d "$work/.git"; then
    git clone --quiet --recurse-submodules https://github.com/ToNi3141/RasterIX.git "$work"
else
    git -C "$work" fetch --quiet origin 9269a01c9bd4c3342bfa70ef8e3cdd751c15c6d0
fi
git -C "$work" reset --hard --quiet 9269a01c9bd4c3342bfa70ef8e3cdd751c15c6d0
git -C "$work" submodule update --init --recursive --quiet

# The production RTL source is the vendored dependency closure.  Refuse to
# model a different implementation, then retain upstream-only top/display code.
for part in RasterIX Float 3rdParty; do
    test -d "/src/rtl/rasterix/$part"
    while IFS= read -r -d "" source; do
        relative=${source#"/src/rtl/rasterix/$part/"}
        target="$work/rtl/$part/$relative"
        test -f "$target"
        cmp -s "$source" "$target"
    done < <(find "/src/rtl/rasterix/$part" -type f -print0)
    cp -a "/src/rtl/rasterix/$part/." "$work/rtl/$part/"
done
python3 /src/test/rasterix/patch_top.py "$work/rtl/top/Verilator/top.v" /work/top.v --tmu-count 1

build_one() {
    tmu=$1
    obj="/cache/vobj-tmu$tmu"
    build="/cache/build-tmu$tmu"
    top="/cache/top-tmu$tmu.v"
    python3 /src/test/rasterix/patch_top.py "$work/rtl/top/Verilator/top.v" "$top" --tmu-count "$tmu"
    verilator -Wno-SELRANGE -Wno-lint -Wno-WIDTH -DUNITTEST -DVARIANT=if -CFLAGS "-O3 -DNDEBUG" \
        -DFRAMEBUFFER_SIZE_IN_PIXEL_LG=14 --cc "$top" --top-module top --Mdir "$obj" \
        -I"$work/rtl/RasterIX" -I"$work/rtl/3rdParty" -I"$work/rtl/Float/rtl/float" \
        -I"$work/rtl/3rdParty/verilog-axi" -I"$work/rtl/3rdParty/verilog-axis" -I"$work/rtl/Display"
    make -s -j"$(nproc)" -C "$obj" -f Vtop.mk
    cmake -S /src/test/rasterix -B "$build" -DRASTERIX_ROOT="$work" -DVERILATOR_OBJDIR="$obj" -DTMU_COUNT="$tmu" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$build" --parallel "$(nproc)"
    timeout --foreground 45m "$build/rasterix_harness" --tmu "$tmu" --output /out
}

build_one 1
build_one 2
for ppm in /out/frames/*.ppm; do
    python3 /src/test/rasterix/ppm_to_png.py "$ppm" "${ppm%.ppm}.png"
    rm "$ppm"
done
python3 - <<"PY"
import json
from pathlib import Path
print("\nRasterIX simulation results (100 MHz):")
print("TMU  scene             fps       command bytes  front-end x86 ms  bands  average cycles/band")
for path in sorted(Path("/out").glob("results-tmu*.json")):
    doc = json.loads(path.read_text())
    for scene in doc["scenes"]:
        tmu, name = doc["tmu_count"], scene["name"]
        fps, commands, frontend, bands, per_band = scene["fps_at_100mhz"], scene["command_bytes"], scene["frontend_ms_x86"], scene["bands"], scene["average_cycles_per_band"]
        print(f"{tmu:>3}  {name:<16} {fps:>8.3f} {commands:>14} {frontend:>18.6f} {bands:>6} {per_band:>20.3f}")
PY
'
