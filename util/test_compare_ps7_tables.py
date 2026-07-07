#!/usr/bin/env python3
# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Self-test for compare_ps7_tables.py: synthesize a blob with two valid
# opcode streams, one too-short stream, and surrounding noise; check the
# decoder finds exactly the valid streams and that self-diff is clean.
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare_ps7_tables as cpt


def words(*ws):
    return b"".join(struct.pack("<I", w) for w in ws)


def maskwrite(addr, mask, val):
    return words(0x33, addr, mask, val)


def stream(n_ops, extra=b""):
    body = b"".join(
        maskwrite(0xF8000100 + 4 * i, 0xFFFFFFFF, i) for i in range(n_ops))
    return body + extra + words(0x00)  # EMIT_EXIT


def write_blob(blob):
    fd, path = tempfile.mkstemp(suffix=".bin")
    with os.fdopen(fd, "wb") as f:
        f.write(blob)
    return path


def main():
    s1 = stream(12)                                       # valid: 12 ops
    s2 = stream(10, extra=words(0x42, 0xF8006054, 0x1))   # valid: 11 ops
    short = stream(3)                                     # rejected: < MIN_OPS
    blob = (b"\x11\x22\x33\x44" * 5) + s1 + words(0, 0) + short + s2 + b"\xff" * 7

    # Same op multiset as s1, but the first two ops swapped: init order is
    # semantically important (PLL/DDR sequencing), so this must NOT pass.
    s1_swapped = (maskwrite(0xF8000104, 0xFFFFFFFF, 1)
                  + maskwrite(0xF8000100, 0xFFFFFFFF, 0)
                  + b"".join(maskwrite(0xF8000100 + 4 * i, 0xFFFFFFFF, i)
                             for i in range(2, 12))
                  + words(0x00))
    blob_reordered = (b"\x11\x22\x33\x44" * 5) + s1_swapped + words(0, 0) \
        + short + s2 + b"\xff" * 7

    # An extra AFI0-style op that --ignore must be able to waive.
    s1_extra = stream(12, extra=words(0x33, 0xF8008000, 1, 1))
    blob_extra = (b"\x11\x22\x33\x44" * 5) + s1_extra + words(0, 0) \
        + short + s2 + b"\xff" * 7

    paths = [write_blob(b) for b in (blob, blob_reordered, blob_extra)]
    path, path_reordered, path_extra = paths
    try:
        streams = cpt.decode(path)
        assert len(streams) == 2, [len(s) for s in streams]
        assert len(streams[0]) == 12
        assert len(streams[1]) == 11
        assert streams[1][-1] == ("MASKPOLL", 0xF8006054, 0x1)
        assert cpt.main([path, path]) == 0          # self-diff: identical
        assert cpt.main([path, path_reordered]) == 1  # reorder must fail
        assert cpt.main([path, path_extra]) == 1      # extra op must fail
        assert cpt.main([path, path_extra,             # ...unless ignored
                         "--ignore", "0xF8008000"]) == 0
    finally:
        for p in paths:
            os.unlink(p)
    print("PASS")


if __name__ == "__main__":
    main()
