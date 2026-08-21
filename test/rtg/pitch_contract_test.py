#!/usr/bin/env python3
"""Keep RTG dispatcher pitch units aligned with the gfx destination ABI."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "ZZ9000_proto.sdk" / "ZZ9000OS" / "src"
DMA = (SRC / "dma_rtg.c").read_text(encoding="utf-8")
MAIN = (SRC / "main.c").read_text(encoding="utf-8")


def case_block(source: str, start: str, end: str) -> str:
    start_at = source.index(start)
    return source[start_at : source.index(end, start_at)]


def require_pitch(source: str, start: str, end: str, setter: str) -> None:
    block = case_block(source, start, end)
    other = "set_fb_bytes(" if setter == "set_fb_words(" else "set_fb_words("
    if setter not in block or other in block:
        raise SystemExit(f"wrong RTG pitch setter in {start.strip()}")


for source in (DMA, MAIN):
    if "set_fb(" in source:
        raise SystemExit("ambiguous set_fb call remains in an RTG dispatcher")

for name, source in (("DMA", DMA), ("register", MAIN)):
    if source.count("set_fb_words(") != 6 or source.count("set_fb_bytes(") != 1:
        raise SystemExit(f"unexpected {name} RTG pitch-setter call set")

for start, end, setter in (
    ("case OP_DRAWLINE:", "case OP_FILLRECT:", "set_fb_words("),
    ("case OP_FILLRECT:", "case OP_COPYRECT:", "set_fb_words("),
    ("case OP_COPYRECT:", "case OP_RECT_PATTERN:", "set_fb_words("),
    ("case OP_RECT_PATTERN:", "case OP_P2C:", "set_fb_bytes("),
    ("case OP_P2C:", "case OP_WRITE_YUV:", "set_fb_words("),
    ("case OP_WRITE_YUV:", "case OP_VIDEO_OVERLAY:", "set_fb_words("),
    ("case OP_INVERTRECT:", "case OP_SPRITE_XY:", "set_fb_words("),
):
    require_pitch(DMA, start, end, setter)

for start, end, setter in (
    ("case REG_ZZ_FILLRECT:", "case REG_ZZ_COPYRECT:", "set_fb_words("),
    ("case REG_ZZ_COPYRECT:", "case REG_ZZ_FILLTEMPLATE:", "set_fb_words("),
    ("case REG_ZZ_FILLTEMPLATE:", "case REG_ZZ_SCRATCH_COPY:", "set_fb_bytes("),
    ("case REG_ZZ_P2C:", "case REG_ZZ_P2D:", "set_fb_words("),
    ("case REG_ZZ_P2D:", "case REG_ZZ_DRAWLINE:", "set_fb_words("),
    ("case REG_ZZ_DRAWLINE:", "case REG_ZZ_INVERTRECT:", "set_fb_words("),
    ("case REG_ZZ_INVERTRECT:", "case REG_ZZ_SET_SPLIT_POS:", "set_fb_words("),
):
    require_pitch(MAIN, start, end, setter)

print("RTG dispatcher pitch contract checks passed")
