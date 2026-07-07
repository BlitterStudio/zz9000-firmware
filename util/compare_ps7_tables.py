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
                                               ordered per-stream diff

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


def fmt(op):
    return op[0] + " " + " ".join("0x%08X" % a for a in op[1:])


def diff_streams(old_streams, new_streams, ignored):
    """Ordered, per-stream diff. Op order is semantically important
    WITHIN a table (ps7_config executes a table's ops in sequence), so
    each stream is compared as a sequence, not a multiset. The position
    of the tables inside the binary, however, is compiler/linker
    layout and carries no meaning — so streams are first paired by
    content similarity, then diffed in order. Ops targeting an
    --ignore'd address are dropped from both sides BEFORE diffing so
    waived deltas do not break alignment. Returns the number of
    differing ops reported."""
    import difflib
    olds = [[op for op in s if op[1] not in ignored] for s in old_streams]
    news = [[op for op in s if op[1] not in ignored] for s in new_streams]

    # Greedy best-similarity pairing (identical tables pair at 1.0
    # first, then near-identical ones like a table with a waived or
    # genuinely changed op).
    ratios = []
    for i, sa in enumerate(olds):
        for j, sb in enumerate(news):
            r = difflib.SequenceMatcher(a=sa, b=sb, autojunk=False).ratio()
            ratios.append((r, i, j))
    ratios.sort(key=lambda t: (-t[0], t[1], t[2]))
    old_pair, new_pair = {}, {}
    for r, i, j in ratios:
        if i not in old_pair and j not in new_pair:
            old_pair[i] = j
            new_pair[j] = i

    diffs = 0
    if len(olds) != len(news):
        print("stream count differs: old=%d new=%d" % (len(olds), len(news)))
    for i, sa in enumerate(olds):
        j = old_pair.get(i)
        if j is None:
            for op in sa:
                print("unmatched OLD stream %d: %s" % (i, fmt(op)))
                diffs += 1
            continue
        sb = news[j]
        sm = difflib.SequenceMatcher(a=sa, b=sb, autojunk=False)
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag == "equal":
                continue
            for op in sa[i1:i2]:
                print("stream old#%d/new#%d: only in OLD: %s"
                      % (i, j, fmt(op)))
                diffs += 1
            for op in sb[j1:j2]:
                print("stream old#%d/new#%d: only in NEW: %s"
                      % (i, j, fmt(op)))
                diffs += 1
    for j, sb in enumerate(news):
        if j not in new_pair:
            for op in sb:
                print("unmatched NEW stream %d: %s" % (j, fmt(op)))
                diffs += 1
    return diffs


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

    old_streams, new_streams = decode(paths[0]), decode(paths[1])
    diffs = diff_streams(old_streams, new_streams, ignored)
    print("ops: old=%d new=%d; differing (non-ignored) ops: %d"
          % (sum(len(s) for s in old_streams),
             sum(len(s) for s in new_streams), diffs))
    return 1 if diffs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
