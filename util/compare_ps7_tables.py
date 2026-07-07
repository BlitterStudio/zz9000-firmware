#!/usr/bin/env python3
# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode and diff the ps7_init register-op tables inside FSBL images.

The Zynq PS init tables (ps7_init_gpl.c) are arrays of encoded ops:
word = (opcode << 4) | nargs; EXIT=0x00, CLEAR=0x11, WRITE=0x22,
MASKWRITE=0x33, MASKPOLL=0x42, MASKDELAY=0x52; the first argument is
always a register address >= 0xE0000000. This tool finds and decodes
those streams in any FSBL image — a real linked ELF (PT_LOAD segments),
the historical flat-binary FSBL_exec.elf wrapper, or a raw .bin — so no
symbol table is required.

Usage:
  compare_ps7_tables.py IMAGE                  list decoded streams
  compare_ps7_tables.py OLD NEW [--ignore ADDR]...
                                               diff op multisets

Exit codes: 0 identical (after --ignore filtering), 1 differences,
2 usage error.
"""
import struct
import sys

# opcode word -> (name, argument count)
OPCODES = {
    0x00: ("EXIT", 0),
    0x11: ("CLEAR", 1),
    0x22: ("WRITE", 2),
    0x33: ("MASKWRITE", 3),
    0x42: ("MASKPOLL", 2),
    0x52: ("MASKDELAY", 2),
}
MIN_OPS = 10             # real tables have dozens of ops; rejects noise
ADDR_FLOOR = 0xE0000000  # all ps7 targets are high MMIO


def load_blobs(path):
    """Return the byte blobs to scan: PT_LOAD segments if ELF, else file."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF":
        return [data]
    (phoff,) = struct.unpack_from("<I", data, 0x1C)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x2A)
    blobs = []
    for i in range(phnum):
        ptype, off, _va, _pa, filesz = struct.unpack_from(
            "<5I", data, phoff + i * phentsize)
        if ptype == 1 and filesz:  # PT_LOAD with file contents
            blobs.append(data[off:off + filesz])
    return blobs or [data]


def parse_stream(words, i):
    """Parse an opcode stream at word index i -> (ops, next_i) or None."""
    ops = []
    n = len(words)
    while i < n:
        w = words[i]
        if w not in OPCODES:
            return None
        name, nargs = OPCODES[w]
        if name == "EXIT":
            return (ops, i + 1) if len(ops) >= MIN_OPS else None
        if i + nargs >= n:
            return None
        args = tuple(words[i + 1:i + 1 + nargs])
        if args[0] < ADDR_FLOOR:
            return None
        ops.append((name,) + args)
        i += 1 + nargs
    return None


def decode(path):
    """Return the list of opcode streams found in the image."""
    streams = []
    for blob in load_blobs(path):
        nwords = len(blob) // 4
        words = list(struct.unpack("<%dI" % nwords, blob[:nwords * 4]))
        i = 0
        while i < len(words):
            hit = parse_stream(words, i)
            if hit:
                streams.append(hit[0])
                i = hit[1]
            else:
                i += 1
    return streams


def op_counts(streams):
    counts = {}
    for s in streams:
        for op in s:
            counts[op] = counts.get(op, 0) + 1
    return counts


def fmt(op):
    return op[0] + " " + " ".join("0x%08X" % a for a in op[1:])


def main(argv):
    paths = []
    ignored = set()
    i = 0
    while i < len(argv):
        if argv[i] == "--ignore":
            if i + 1 >= len(argv):
                print("--ignore needs an address", file=sys.stderr)
                return 2
            ignored.add(int(argv[i + 1], 0))
            i += 2
        else:
            paths.append(argv[i])
            i += 1

    if len(paths) == 1:
        streams = decode(paths[0])
        for n, s in enumerate(streams):
            print("stream %d: %d ops" % (n, len(s)))
            for op in s:
                print("  " + fmt(op))
        print("%d streams" % len(streams))
        return 0

    if len(paths) != 2:
        print(__doc__, file=sys.stderr)
        return 2

    old, new = (op_counts(decode(p)) for p in paths)
    diffs = 0
    for op in sorted(set(old) | set(new)):
        if op[1] in ignored:
            continue
        a, b = old.get(op, 0), new.get(op, 0)
        if a != b:
            print("%-56s old=%d new=%d" % (fmt(op), a, b))
            diffs += 1
    print("ops: old=%d new=%d; differing (non-ignored) op kinds: %d"
          % (sum(old.values()), sum(new.values()), diffs))
    return 1 if diffs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
