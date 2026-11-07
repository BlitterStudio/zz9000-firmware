#!/usr/bin/env bash
# Diagnostic-only: execute RasterIX's documented Verilator make target without
# changing upstream sources.  Kept separate from run.sh so the harness itself
# remains reproducible from its pinned source closure.
set -euo pipefail

readonly RASTERIX_REV=9269a01c9bd4c3342bfa70ef8e3cdd751c15c6d0
export MSYS_NO_PATHCONV=1
docker run --rm --entrypoint /usr/bin/timeout verilator/verilator:latest \
    --foreground 45m bash -lc '
        set -euo pipefail
        apt-get update
        DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends qtbase5-dev xvfb
        verilator --version
        git clone --depth 1 --recurse-submodules https://github.com/ToNi3141/RasterIX.git /tmp/RasterIX
        git -C /tmp/RasterIX fetch --depth 1 origin '"$RASTERIX_REV"'
        git -C /tmp/RasterIX checkout --detach '"$RASTERIX_REV"'
        git -C /tmp/RasterIX submodule update --init --recursive
        make -C /tmp/RasterIX/rtl/top/Verilator -f Makefile.linux rixif -j"$(nproc)"
        cd /tmp/RasterIX/example/qtDebug/qtRasterizerFpga
        qmake -after TARGET_BUILD=simulation VERILATOR_PATH=/usr/local/share/verilator qtRasterizerFpga.pro
        make -j2
        set +e
        xvfb-run -a timeout 10s ./qtRasterizerFpga
        result=$?
        set -e
        test "$result" = 124
    '
