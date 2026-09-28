#!/usr/bin/env python3
# Copyright (C) 2026, Dimitris Panokostas / BlitterStudio
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the classic AmigaOS icons for zzplay.

The .info files are committed binaries, so this script exists to make them
reproducible and reviewable: run it and the committed bytes must come back
identical. It emits plain OS 1.x/3.x DiskObject icons (no NewIcon/GlowIcon
chunk) because those render on every system zzplay supports.

Layout reference: struct DiskObject (78 bytes) followed, in order, by the
optional DrawerData, the render Image, the select Image, do_DefaultTool,
do_ToolTypes and do_ToolWindow bodies.

Usage: python3 scripts/make-zzplay-icons.py [output-directory]
"""

import os
import struct
import sys

WIDTH = 46
HEIGHT = 46
DEPTH = 2

WB_TOOL = 3
WB_PROJECT = 4

GFLG_GADGIMAGE = 0x0004
GACT_RELVERIFY = 0x0001
GTYP_BOOLGADGET = 0x0001


def blank():
    return [[0] * WIDTH for _ in range(HEIGHT)]


def fill_rect(pix, x0, y0, x1, y1, colour):
    for y in range(max(0, y0), min(HEIGHT, y1)):
        for x in range(max(0, x0), min(WIDTH, x1)):
            pix[y][x] = colour


def frame_rect(pix, x0, y0, x1, y1, colour):
    for x in range(x0, x1):
        pix[y0][x] = colour
        pix[y1 - 1][x] = colour
    for y in range(y0, y1):
        pix[y][x0] = colour
        pix[y][x1 - 1] = colour


def play_triangle(pix, cx, cy, half, colour):
    """A centred right-pointing triangle: the universal 'play' mark."""
    for dy in range(-half, half + 1):
        span = half - abs(dy)
        for dx in range(0, span + 1):
            x = cx - half // 2 + dx
            y = cy + dy
            if 0 <= x < WIDTH and 0 <= y < HEIGHT:
                pix[y][x] = colour


def draw(selected):
    """Colours are Workbench pens: 0 grey, 1 black, 2 white, 3 blue."""
    pix = blank()
    # Monitor body.
    fill_rect(pix, 3, 5, 43, 34, 3)
    frame_rect(pix, 3, 5, 43, 34, 1)
    # Screen.
    fill_rect(pix, 6, 8, 40, 31, 1 if not selected else 2)
    play_triangle(pix, 23, 19, 8, 2 if not selected else 1)
    # Stand.
    fill_rect(pix, 19, 34, 27, 39, 1)
    fill_rect(pix, 12, 39, 34, 42, 3)
    frame_rect(pix, 12, 39, 34, 42, 1)
    return pix


def to_planes(pix):
    """Amiga planar: one bitplane at a time, rows padded to a 16-bit word."""
    words = (WIDTH + 15) // 16
    row_bytes = words * 2
    out = bytearray()
    for plane in range(DEPTH):
        for y in range(HEIGHT):
            row = bytearray(row_bytes)
            for x in range(WIDTH):
                if (pix[y][x] >> plane) & 1:
                    row[x // 8] |= 0x80 >> (x % 8)
            out += row
    return bytes(out)


def image_struct(data_present=True):
    return struct.pack(
        ">hhhhhIBBI",
        0, 0, WIDTH, HEIGHT, DEPTH,
        1 if data_present else 0,   # ImageData pointer: non-zero marker
        (1 << DEPTH) - 1,           # PlanePick
        0,                          # PlaneOnOff
        0,                          # NextImage
    )


def gadget(select_render):
    return struct.pack(
        ">IhhhhHHHIIIIIhI",
        0,                    # ga_Next
        0, 0,                 # ga_LeftEdge, ga_TopEdge
        WIDTH, HEIGHT,        # ga_Width, ga_Height
        GFLG_GADGIMAGE,
        GACT_RELVERIFY,
        GTYP_BOOLGADGET,
        1,                    # ga_GadgetRender: non-zero marker
        1 if select_render else 0,
        0,                    # ga_GadgetText
        0,                    # ga_MutualExclude
        0,                    # ga_SpecialInfo
        0,                    # ga_GadgetID
        0,                    # ga_UserData
    )


def string_body(text):
    raw = text.encode("latin-1") + b"\0"
    return struct.pack(">I", len(raw)) + raw


def tooltypes_body(entries):
    # A LONG holding (count + 1) * 4, then each entry as length + bytes.
    out = struct.pack(">I", (len(entries) + 1) * 4)
    for entry in entries:
        out += string_body(entry)
    return out


def disk_object(icon_type, default_tool, tooltypes, stack):
    body = struct.pack(">HH", 0xE310, 1)
    body += gadget(select_render=True)
    body += struct.pack(">BB", icon_type, 0)
    body += struct.pack(">I", 1 if default_tool else 0)
    body += struct.pack(">I", 1 if tooltypes else 0)
    # NO_ICON_POSITION lets Workbench place the icon itself.
    body += struct.pack(">II", 0x80000000, 0x80000000)
    body += struct.pack(">I", 0)   # do_DrawerData
    body += struct.pack(">I", 0)   # do_ToolWindow
    body += struct.pack(">I", stack)
    assert len(body) == 78, len(body)
    return body


def build(icon_type, default_tool, tooltypes, stack):
    normal = to_planes(draw(selected=False))
    select = to_planes(draw(selected=True))
    out = disk_object(icon_type, default_tool, tooltypes, stack)
    out += image_struct() + normal
    out += image_struct() + select
    if default_tool:
        out += string_body(default_tool)
    if tooltypes:
        out += tooltypes_body(tooltypes)
    return out


# Disabled by default so the icon documents every option without changing
# behaviour; the user removes the parentheses to enable one.
TOOLTYPES = [
    "(AUDIO=AUTO)",
    "(FULLSCREEN)",
    "(LOOP)",
    "(QUIET)",
    "(VERBOSE)",
    "(FPS)",
    "(BENCHMARK)",
]


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "amiga/icons"
    os.makedirs(out_dir, exist_ok=True)
    targets = [
        ("zzplay.info", WB_TOOL, None),
        ("zzplay-project.info", WB_PROJECT, "ZZPlay"),
    ]
    for name, icon_type, default_tool in targets:
        data = build(icon_type, default_tool, TOOLTYPES, 16384)
        path = os.path.join(out_dir, name)
        with open(path, "wb") as handle:
            handle.write(data)
        print("wrote %s (%d bytes)" % (path, len(data)))


if __name__ == "__main__":
    main()
