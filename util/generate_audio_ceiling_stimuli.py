#!/usr/bin/env python3
"""Generate deterministic ZZ9000AX ceiling-measurement source tones."""

import argparse
import hashlib
import math
import struct
import wave
from pathlib import Path

AHI_RATE = 48000
AHI_SECONDS = 20
AHI_AMPLITUDE = round(0.99 * 32767)
PAULA_RATE = 16000
PAULA_AMPLITUDE = 126
TONE_HZ = 1000


def write_ahi_wave(path):
    with wave.open(str(path), "wb") as output:
        output.setnchannels(2)
        output.setsampwidth(2)
        output.setframerate(AHI_RATE)
        for start in range(0, AHI_RATE * AHI_SECONDS, AHI_RATE):
            frames = bytearray()
            for index in range(start, start + AHI_RATE):
                sample = round(AHI_AMPLITUDE * math.sin(
                    2.0 * math.pi * TONE_HZ * index / AHI_RATE))
                frames.extend(struct.pack("<hh", sample, sample))
            output.writeframesraw(frames)


def iff_chunk(tag, data):
    padding = b"\0" if len(data) & 1 else b""
    return tag + struct.pack(">I", len(data)) + data + padding


def write_paula_8svx(path):
    body = bytes(
        round(PAULA_AMPLITUDE * math.sin(
            2.0 * math.pi * TONE_HZ * index / PAULA_RATE)) & 0xFF
        for index in range(PAULA_RATE)
    )
    vhdr = struct.pack(
        ">IIIHBBI", 0, len(body), PAULA_RATE // TONE_HZ,
        PAULA_RATE, 1, 0, 0x10000)
    chunks = (
        iff_chunk(b"VHDR", vhdr)
        + iff_chunk(b"NAME", b"ZZ9000AX 1kHz Paula full-scale")
        + iff_chunk(b"BODY", body)
    )
    path.write_bytes(
        b"FORM" + struct.pack(">I", 4 + len(chunks)) + b"8SVX" + chunks)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output_dir", nargs="?", default=".")
    args = parser.parse_args()
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    ahi = output_dir / "ceiling_1khz_ahi_0dbfs.wav"
    paula = output_dir / "ceiling_1khz_paula.8svx"
    write_ahi_wave(ahi)
    write_paula_8svx(paula)
    for path in (ahi, paula):
        print("%s  %s" % (digest(path), path))


if __name__ == "__main__":
    main()
