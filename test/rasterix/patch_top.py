#!/usr/bin/env python3
"""Apply the ZZ9000 simulation parameters to RasterIX's upstream Verilator top."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("input", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--tmu-count", type=int, choices=(1, 2), required=True)
parser.add_argument("--band-lg", type=int, required=True)
parser.add_argument("--framebuffer-sub-pixel-width", type=int, choices=(5, 6), required=True)
parser.add_argument("--stencil", type=int, choices=(0, 1), required=True)
args = parser.parse_args()

source = args.input.read_text()
replacements = {
    ".FRAMEBUFFER_SIZE_IN_PIXEL_LG(FRAMEBUFFER_SIZE_IN_PIXEL_LG),": f".FRAMEBUFFER_SIZE_IN_PIXEL_LG({args.band_lg}),",
    ".FRAMEBUFFER_SUB_PIXEL_WIDTH(6),": f".FRAMEBUFFER_SUB_PIXEL_WIDTH({args.framebuffer_sub_pixel_width}),",
    ".ENABLE_STENCIL_BUFFER(1),": f".ENABLE_STENCIL_BUFFER({args.stencil}),",
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
print(
    f"RasterIX RTL: variant=if, band_lg={args.band_lg}, color=RGB{args.framebuffer_sub_pixel_width * 3}, depth=Z16, stencil={'on' if args.stencil else 'off'}, "
    f"texture_filtering=on, max_texture=256, tmu={args.tmu_count}, "
)
