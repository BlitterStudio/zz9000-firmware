#!/usr/bin/env python3
"""Convert the harness's binary PPM frame into a standards-compliant RGB PNG."""
import argparse
import struct
import zlib
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("input", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()

with args.input.open("rb") as source:
    if source.readline().strip() != b"P6":
        raise SystemExit("expected binary PPM")
    dimensions = source.readline()
    while dimensions.startswith(b"#"):
        dimensions = source.readline()
    width, height = map(int, dimensions.split())
    if source.readline().strip() != b"255":
        raise SystemExit("expected 8-bit PPM")
    pixels = source.read()
if len(pixels) != width * height * 3:
    raise SystemExit("truncated PPM")

def chunk(kind, payload):
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff)

scanlines = b"".join(b"\0" + pixels[row * width * 3:(row + 1) * width * 3] for row in range(height))
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(scanlines, 9)) + chunk(b"IEND", b"")
args.output.write_bytes(png)
