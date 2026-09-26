#!/usr/bin/env python3
"""Verify that A4000 release ZIPs contain the routed C28 FPGA images."""

import array
import hashlib
import json
from pathlib import Path
import sys
import zipfile


ROOT = Path(__file__).resolve().parents[2]
BUILDS = ROOT / "bootimage_work/variants/a4000-c28-builds.json"


def fpga_payload(bitstream):
    position = 2 + int.from_bytes(bitstream[:2], "big")
    if bitstream[position:position + 2] != b"\x00\x01":
        raise ValueError("invalid Vivado bitstream header")
    position += 2
    while position < len(bitstream):
        tag = bitstream[position:position + 1]
        position += 1
        size = 4 if tag == b"e" else 2
        length = int.from_bytes(bitstream[position:position + size], "big")
        position += size
        data = bitstream[position:position + length]
        if len(data) != length:
            raise ValueError("truncated Vivado bitstream")
        position += length
        if tag == b"e":
            if length % 4:
                raise ValueError("unaligned FPGA payload")
            words = array.array("I")
            words.frombytes(data)
            if sys.byteorder == "little":
                words.byteswap()
            return words.tobytes()
    raise ValueError("missing FPGA payload")


def verify_variant(output_dir, tag, variant, expected):
    bit_file = ROOT / "bootimage_work/variants" / f"zz9000_ps_wrapper-{variant}.bit"
    bitstream = bit_file.read_bytes()
    digest = hashlib.sha256(bitstream).hexdigest()
    if digest != expected["sha256"]:
        raise ValueError(f"{variant}: committed bitstream differs from routed C28 candidate")

    archive_name = f"zz9000-firmware-{tag}-{variant}"
    archive_file = output_dir / (archive_name + ".zip")
    with zipfile.ZipFile(archive_file) as archive:
        damaged = archive.testzip()
        if damaged is not None:
            raise ValueError(f"{variant}: corrupt ZIP member {damaged}")
        boot = archive.read(f"{archive_name}/BOOT.bin")

    payload = fpga_payload(bitstream)
    offset = boot.find(payload)
    if offset < 0 or boot.find(payload, offset + 1) >= 0:
        raise ValueError(f"{variant}: BOOT.bin does not contain exactly one expected C28 FPGA payload")
    print(f"PASS {variant}: routed C28 bitstream {digest}, BOOT offset {offset:#x}")


def main():
    if len(sys.argv) != 3:
        raise SystemExit("Usage: verify_a4000_c28_assets.py OUTPUT_DIR BUILD_LABEL")
    output_dir, tag = Path(sys.argv[1]), sys.argv[2]
    variants = json.loads(BUILDS.read_text(encoding="utf-8"))["bitstreams"]
    for variant, expected in variants.items():
        verify_variant(output_dir, tag, variant, expected)


if __name__ == "__main__":
    main()
