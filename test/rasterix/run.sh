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
PROFILE=${RASTERIX_PROFILE:-zz9000}
REVISION=${RASTERIX_REVISION:-9269a01c9bd4c3342bfa70ef8e3cdd751c15c6d0}
MINIGL_ROOT=${MINIGL_ROOT:-D:/Github/zz9000-minigl-worktree/ZZ9000_proto.sdk/ZZ9000OS/src}
MINIGL_DOCKER=$(cygpath -aw "$MINIGL_ROOT")
docker run --rm \
    -e RASTERIX_PROFILE="$PROFILE" \
    -e RASTERIX_REVISION="$REVISION" \
    -e RASTERIX_EXPERIMENT="${RASTERIX_EXPERIMENT:-}" \
    -v "$ROOT_DOCKER:/src:ro" \
    -v "$OUTPUT_DOCKER:/out" \
    -v "$MINIGL_DOCKER:/minigl:ro" \
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
    git -C "$work" fetch --quiet origin
fi
git -C "$work" reset --hard --quiet "$RASTERIX_REVISION"
git -C "$work" submodule foreach --recursive "git reset --hard --quiet"
git -C "$work" submodule update --init --recursive --quiet
revision=$(git -C "$work" rev-parse --short HEAD)

if test "$RASTERIX_REVISION" = 9269a01c9bd4c3342bfa70ef8e3cdd751c15c6d0; then
    # The production RTL source is the vendored dependency closure. Refuse to
    # model a different implementation, then retain upstream-only top/display code.
    for part in RasterIX Float 3rdParty; do
        test -d "/src/rtl/rasterix/$part"
        while IFS= read -r -d "" source; do
            relative=${source#"/src/rtl/rasterix/$part/"}
            target="$work/rtl/$part/$relative"
            test -f "$target"
            case "$part/$relative" in
                Float/rtl/float/FloatToInt.v|RasterIX/TexEnv.v|RasterIX/TextureReaderContext.v|RasterIX/RasterIXCoreIF.v|RasterIX/InternalFramebufferCommandHandler.v|RasterIX/CommandParser.v|RasterIX/RasterIXCoreEF.v|RasterIX/StreamConcatFifo.v)
                    continue
                    ;;
            esac
            cmp -s "$source" "$target"
        done < <(find "/src/rtl/rasterix/$part" -type f -print0)
        cp -a "/src/rtl/rasterix/$part/." "$work/rtl/$part/"
    done
if test "${RASTERIX_EXPERIMENT:-}" = e1; then
    python3 /src/test/rasterix/patch_crossbar.py "$work/rtl/RasterIX/AxisToAxiCrossbar.v"
fi

fi


case "${RASTERIX_PROFILE:-zz9000}" in
zz9000)
    band_lg=14; width=640; height=480; tmu_list="1 2"; sub_pixel_width=5; stencil=0; use_upstream_top=0
    pages=256; textures=256; page_bytes=4096; threaded=false; display_list_size="1024*1024*4"
    ;;
upstream-nexys-rixif)
    band_lg=17; width=1024; height=600; tmu_list=2; sub_pixel_width=6; stencil=1; use_upstream_top=0
    pages=12288; textures=12288; page_bytes=2048; threaded=true; display_list_size="1024*1024*16"
    ;;
upstream-verilator)
    band_lg=15; width=640; height=480; tmu_list=2; sub_pixel_width=6; stencil=1; use_upstream_top=1
    pages=6912; textures=6912; page_bytes=4096; threaded=true; display_list_size=1048576
    ;;
*)
    echo "unknown RASTERIX_PROFILE: $RASTERIX_PROFILE" >&2
    exit 2
    ;;
esac

build_one() {
    tmu=$1
    obj="/cache/vobj-${RASTERIX_PROFILE}-${revision}-tmu$tmu"
    build="/cache/build-${RASTERIX_PROFILE}-${revision}-tmu$tmu"
    top="/cache/top-tmu$tmu.v"
    c_backend_args="-DC_UNTEXTURED=false -DC_TRACE=false"
    trace_args=
    verilator_args=
    if test "${RASTERIX_EXPERIMENT:-}" = e3; then
        c_backend_args="-DC_UNTEXTURED=true -DC_TRACE=true"
        trace_args=--trace
        verilator_args="--x-assign unique --x-initial unique"
    fi
    if test "$use_upstream_top" = 1; then
        top="$work/rtl/top/Verilator/top.v"
    else
        python3 /src/test/rasterix/patch_top.py "$work/rtl/top/Verilator/top.v" "$top" --tmu-count "$tmu" \
            --band-lg "$band_lg" --framebuffer-sub-pixel-width "$sub_pixel_width" --stencil "$stencil"
    fi
    verilator -Wno-SELRANGE -Wno-lint -Wno-WIDTH -DUNITTEST -DVARIANT=if -CFLAGS "-O3 -DNDEBUG" $trace_args $verilator_args \
        -DFRAMEBUFFER_SIZE_IN_PIXEL_LG="$band_lg" --cc "$top" --top-module top --Mdir "$obj" \
        -I"$work/rtl/RasterIX" -I"$work/rtl/3rdParty" -I"$work/rtl/Float/rtl/float" \
        -I"$work/rtl/3rdParty/verilog-axi" -I"$work/rtl/3rdParty/verilog-axis" -I"$work/rtl/Display"
    make -s -j"$(nproc)" -C "$obj" -f Vtop.mk
    cmake -S /src/test/rasterix -B "$build" -DRASTERIX_ROOT="$work" -DVERILATOR_OBJDIR="$obj" -DTMU_COUNT="$tmu" \
        -DRIX_CORE_MAX_DISPLAY_WIDTH="$width" -DRIX_CORE_MAX_DISPLAY_HEIGHT="$height" -DRIX_CORE_FRAMEBUFFER_SIZE_IN_PIXEL_LG="$band_lg" \
        -DRIX_CORE_NUMBER_OF_TEXTURE_PAGES="$pages" -DRIX_CORE_NUMBER_OF_TEXTURES="$textures" -DRIX_CORE_TEXTURE_PAGE_SIZE="$page_bytes" \
        -DRIX_CORE_THREADED_RASTERIZATION="$threaded" -DRIX_CORE_THREADED_RASTERIZATION_DISPLAY_LIST_SIZE="$display_list_size" \
        -DRIX_CORE_COLOR_BUFFER_LOC_0=0x01E00000 -DRIX_CORE_COLOR_BUFFER_LOC_1=0x01E00000 -DRIX_CORE_COLOR_BUFFER_LOC_2=0x01C00000 \
        -DRIX_CORE_DEPTH_BUFFER_LOC=0x01A00000 -DRIX_CORE_STENCIL_BUFFER_LOC=0x01900000 \
        -DRIX_C_ROOT=/minigl $c_backend_args -DCMAKE_BUILD_TYPE=Release
    cmake --build "$build" --parallel "$(nproc)"
    if test "${RASTERIX_EXPERIMENT:-}" = e3; then
        result=0
        for seed in 1 2; do
            if ! timeout --foreground 45m "$build/rasterix_harness" --tmu "$tmu" --output /out --backend "${RASTERIX_BACKEND:-c}" \
                    +verilator+rand+reset+2 "+verilator+seed+$seed"; then
                result=1
            fi
        done
        return "$result"
    fi
    timeout --foreground 45m "$build/rasterix_harness" --tmu "$tmu" --output /out --backend "${RASTERIX_BACKEND:-c}"
}

for tmu in $tmu_list; do
    build_one "$tmu"
done
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
