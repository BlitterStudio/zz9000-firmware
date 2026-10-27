#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the pinned RasterIX C++ reference in an isolated gcc:13 container,
# then compare rix_* command constructors and descriptors against it, and
# assert the verified ZZ9000 band dispatch protocol.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
PIN=9269a01c9bd4c3342bfa70ef8e3cdd751c15c6d0

docker run --rm -v "$ROOT:/src:ro" gcc:13 sh -ec '
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends cmake git >/dev/null
    git clone --quiet --recursive https://github.com/ToNi3141/RasterIX.git /tmp/upstream
    git -C /tmp/upstream checkout --quiet "$0"
    git -C /tmp/upstream submodule update --init --recursive --quiet
    common="-DRIX_CORE_TMU_COUNT=1 -DRIX_CORE_MAX_TEXTURE_SIZE=256 -DRIX_CORE_ENABLE_MIPMAPPING=true -DRIX_CORE_MAX_DISPLAY_WIDTH=640 -DRIX_CORE_MAX_DISPLAY_HEIGHT=480 -DRIX_CORE_FRAMEBUFFER_SIZE_IN_PIXEL_LG=14 -DRIX_CORE_ENABLE_ATTRIBUTE_SCALING=true -DRIX_CORE_NUMBER_OF_TEXTURE_PAGES=1024 -DRIX_CORE_NUMBER_OF_TEXTURES=1024 -DRIX_CORE_TEXTURE_PAGE_SIZE=2048 -DRIX_CORE_GRAM_MEMORY_LOC=0x1c000000 -DRIX_CORE_COLOR_BUFFER_LOC_0=0x07820000 -DRIX_CORE_COLOR_BUFFER_LOC_1=0x07820000 -DRIX_CORE_COLOR_BUFFER_LOC_2=0x07820000 -DRIX_CORE_DEPTH_BUFFER_LOC=0x07b20000 -DRIX_CORE_STENCIL_BUFFER_LOC=0 -DRIX_CORE_THREADED_RASTERIZATION=false -DRIX_CORE_THREADED_RASTERIZATION_DISPLAY_LIST_SIZE=4194304 -DRIX_CORE_ENABLE_VSYNC=false -DRIX_CORE_MAX_VBO_COUNT=256 -DRIX_CORE_AUTOLOAD_INTERNAL_FRAMEBUFFER=false -DRIX_CORE_SOFTWARE_RENDERING=false"
    cmake -S /tmp/upstream -B /tmp/upstream-build -DRIX_BUILD_NATIVE=ON $common >/dev/null
    cmake --build /tmp/upstream-build --target gl -j2 >/dev/null
    gcc -std=c99 -Wall -Wextra -Werror -I/src/ZZ9000_proto.sdk/ZZ9000OS/src -c /src/ZZ9000_proto.sdk/ZZ9000OS/src/rix_cmd.c -o /tmp/rix_cmd.o
    gcc -std=c99 -Wall -Wextra -Werror -I/src/ZZ9000_proto.sdk/ZZ9000OS/src -c /src/ZZ9000_proto.sdk/ZZ9000OS/src/rix_setup.c -o /tmp/rix_setup.o
    gcc -std=c99 -Wall -Wextra -Werror -I/src/ZZ9000_proto.sdk/ZZ9000OS/src -c /src/ZZ9000_proto.sdk/ZZ9000OS/src/rix_texture.c -o /tmp/rix_texture.o
    gcc -std=c99 -Wall -Wextra -Werror -I/src/ZZ9000_proto.sdk/ZZ9000OS/src -c /src/ZZ9000_proto.sdk/ZZ9000OS/src/rix_bin.c -o /tmp/rix_bin.o
    libgl=$(find /tmp/upstream-build -name libgl.a -print -quit)
    test -n "$libgl"
    g++ -std=c++17 -Wall -Wextra -Werror -I/src/ZZ9000_proto.sdk/ZZ9000OS/src /src/test/minigl/rix_frame_differential.cpp /tmp/rix_cmd.o /tmp/rix_setup.o /tmp/rix_bin.o -lm -o /tmp/rix_frame_differential
    /tmp/rix_frame_differential
    g++ -std=c++17 -Wall -Wextra -Werror -DERANGE=34 -I/src/ZZ9000_proto.sdk/ZZ9000OS/src -iquote /tmp/upstream/lib/gl -isystem /tmp/upstream/lib/3rdParty/span/include -isystem /tmp/upstream/lib/stubs/spdlog $common /src/test/minigl/rix_differential.cpp /tmp/upstream/lib/gl/renderer/Rasterizer.cpp /tmp/rix_cmd.o /tmp/rix_setup.o /tmp/rix_texture.o -lm -o /tmp/rix_differential
    /tmp/rix_differential
' "$PIN"
