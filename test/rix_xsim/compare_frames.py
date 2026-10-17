#!/usr/bin/env python3
"""Quantized RGB565 frame comparison for real MiniGL RasterIX xsim captures."""
import argparse
import json
import struct
import zlib
from pathlib import Path


def read_token(data, pos):
    while pos < len(data) and data[pos] in b" \t\r\n":
        pos += 1
    while pos < len(data) and data[pos] == ord("#"):
        while pos < len(data) and data[pos] not in b"\r\n":
            pos += 1
        while pos < len(data) and data[pos] in b" \t\r\n":
            pos += 1
    end = pos
    while end < len(data) and data[end] not in b" \t\r\n":
        end += 1
    return data[pos:end], end


def ppm(path):
    data = Path(path).read_bytes()
    magic, pos = read_token(data, 0)
    width, pos = read_token(data, pos)
    height, pos = read_token(data, pos)
    maximum, pos = read_token(data, pos)
    if magic != b"P6" or maximum != b"255":
        raise ValueError(f"{path}: expected 8-bit binary PPM")
    if pos < len(data) and data[pos] in b" \t\r\n":
        pos += 1
    width, height = int(width), int(height)
    pixels = data[pos:]
    if len(pixels) != width * height * 3:
        raise ValueError(f"{path}: expected {width * height * 3} bytes, got {len(pixels)}")
    return width, height, pixels


def png(path, width, height, pixels):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    rows = b"".join(b"\0" + pixels[row * width * 3:(row + 1) * width * 3] for row in range(height))
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("diff", type=Path)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    width, height, ref = ppm(args.reference)
    cand_width, cand_height, cand = ppm(args.candidate)
    if (width, height) != (cand_width, cand_height):
        raise ValueError(f"frame size mismatch: {width}x{height} versus {cand_width}x{cand_height}")

    sums = [0, 0, 0]
    maxima = [0, 0, 0]
    beyond_one = 0
    coverage = 0
    diff = bytearray(width * height * 3)
    clear = (ref[0] >> 3, ref[1] >> 2, ref[2] >> 3)
    for index in range(width * height):
        base = index * 3
        expected = (ref[base] >> 3, ref[base + 1] >> 2, ref[base + 2] >> 3)
        actual = (cand[base] >> 3, cand[base + 1] >> 2, cand[base + 2] >> 3)
        error = tuple(abs(expected[channel] - actual[channel]) for channel in range(3))
        for channel in range(3):
            sums[channel] += error[channel]
            maxima[channel] = max(maxima[channel], error[channel])
        if max(error) > 1:
            beyond_one += 1
        expected_drawn = expected != clear
        actual_drawn = actual != clear
        if expected_drawn != actual_drawn:
            coverage += 1
            diff[base:base + 3] = b"\xff\xff\x00"
        elif error != (0, 0, 0):
            diff[base] = min(255, error[0] * 32)
            diff[base + 1] = min(255, error[1] * 16)
            diff[base + 2] = min(255, error[2] * 32)
    pixels = width * height
    report = {
        "width": width,
        "height": height,
        "rgb565_lsb": {
            "max": dict(zip(("r5", "g6", "b5"), maxima)),
            "mean": dict(zip(("r5", "g6", "b5"), (value / pixels for value in sums))),
            "pixels_beyond_1_lsb": beyond_one,
            "pixels_beyond_1_lsb_percent": beyond_one * 100.0 / pixels,
            "coverage_mismatches": coverage,
            "coverage_mismatches_percent": coverage * 100.0 / pixels,
        },
        "thresholds": {
            "max_lsb": 1,
            "mean_lsb": 0.02,
            "beyond_1_lsb_percent": 0.25,
            "coverage_mismatches_percent": 0.25,
        },
    }
    metrics = report["rgb565_lsb"]
    limits = report["thresholds"]
    report["pass"] = (max(maxima) <= limits["max_lsb"] and max(metrics["mean"].values()) <= limits["mean_lsb"] and metrics["pixels_beyond_1_lsb_percent"] <= limits["beyond_1_lsb_percent"] and metrics["coverage_mismatches_percent"] <= limits["coverage_mismatches_percent"])
    png(args.diff, width, height, diff)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, sort_keys=True))
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
