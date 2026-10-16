#!/usr/bin/env python3
"""Apply the ZZ9000 simulation parameters to RasterIX's upstream Verilator top."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("input", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--tmu-count", type=int, choices=(1, 2), required=True)
args = parser.parse_args()

source = args.input.read_text()
replacements = {
    ".FRAMEBUFFER_SIZE_IN_PIXEL_LG(FRAMEBUFFER_SIZE_IN_PIXEL_LG),": ".FRAMEBUFFER_SIZE_IN_PIXEL_LG(14),",
    ".ENABLE_STENCIL_BUFFER(1),": ".ENABLE_STENCIL_BUFFER(0),",
    ".ENABLE_DEPTH_BUFFER(1),": ".ENABLE_DEPTH_BUFFER(1),",
    ".ENABLE_TEXTURE_FILTERING(1),": ".ENABLE_TEXTURE_FILTERING(1),",
    ".MAX_TEXTURE_SIZE(MAX_TEXTURE_SIZE),": ".MAX_TEXTURE_SIZE(256),",
    ".TMU_COUNT(2),": f".TMU_COUNT({args.tmu_count}),",
    ".RASTERIZER_ENABLE_FLOAT_INTERPOLATION(0)": ".RASTERIZER_ENABLE_FLOAT_INTERPOLATION(0),\n        .RASTERIZER_FIXPOINT_PRECISION(25)",
}
for old, new in replacements.items():
    if source.count(old) != 1:
        raise SystemExit(f"expected one upstream top.v occurrence: {old}")
    source = source.replace(old, new)
args.output.write_text(source)
