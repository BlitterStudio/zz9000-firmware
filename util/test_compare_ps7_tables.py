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


def main():
    s1 = stream(12)                                       # valid: 12 ops
    s2 = stream(10, extra=words(0x42, 0xF8006054, 0x1))   # valid: 11 ops
    short = stream(3)                                     # rejected: < MIN_OPS
    blob = (b"\x11\x22\x33\x44" * 5) + s1 + words(0, 0) + short + s2 + b"\xff" * 7

    fd, path = tempfile.mkstemp(suffix=".bin")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(blob)
        streams = cpt.decode(path)
        assert len(streams) == 2, [len(s) for s in streams]
        assert len(streams[0]) == 12
        assert len(streams[1]) == 11
        assert streams[1][-1] == ("MASKPOLL", 0xF8006054, 0x1)
        assert cpt.main([path, path]) == 0      # self-diff: identical
    finally:
        os.unlink(path)
    print("PASS")


if __name__ == "__main__":
    main()
