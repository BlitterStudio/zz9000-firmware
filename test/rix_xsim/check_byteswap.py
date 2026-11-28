#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the RGB565 colour byte swap (CSR CONTROL bit 2) between two runs.

usage: check_byteswap.py OFF.json ON.json

OFF and ON are run_tp_scenes.py summaries of the same scenes with
COLOR_BYTE_SWAP unset and set.  The two summaries must list the same
nonempty set of scenes, each exactly once, and every entry must carry the
SHA-256 of its logical frame and the FB_DIGEST values.  For every scene:
  * the logical frame (the testbench un-swaps on dump) is byte-identical;
  * the colour buffer as stored with the swap on equals the swap-off buffer
    with every 16-bit pixel's two bytes exchanged, and vice versa;
  * the depth buffer is identical.
Exits nonzero on any difference or on an incomplete input.
"""
import json
import sys


def load(path, which):
    try:
        rows = json.load(open(path))
    except (OSError, ValueError) as error:
        sys.exit(f"{which} summary {path}: cannot read ({error})")
    if not isinstance(rows, list) or not rows:
        sys.exit(f"{which} summary {path}: empty or not a list")
    table = {}
    for row in rows:
        name = row.get("scene")
        if not name:
            sys.exit(f"{which} summary {path}: entry without scene name")
        if name in table:
            sys.exit(f"{which} summary {path}: duplicate scene {name}")
        table[name] = row
    return table


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    off = load(sys.argv[1], "swap-off")
    on = load(sys.argv[2], "swap-on")
    if set(off) != set(on):
        only_off = sorted(set(off) - set(on))
        only_on = sorted(set(on) - set(off))
        sys.exit("scene sets differ: only in swap-off %s, only in swap-on %s" % (only_off, only_on))
    bad = []
    for name in sorted(off):
        a, b = off[name], on[name]
        checks = {}
        checks["frame sha256 present"] = bool(a.get("sha256")) and bool(b.get("sha256"))
        if checks["frame sha256 present"]:
            checks["frame"] = a["sha256"] == b["sha256"]
        da, db = a.get("digest"), b.get("digest")
        checks["digest present"] = isinstance(da, dict) and isinstance(db, dict) and all(
            key in da and key in db for key in ("swap", "raw", "pairswapped", "depth"))
        if checks["digest present"]:
            checks["modes off/on"] = da["swap"] == 0 and db["swap"] == 1
            hashes_known = all(isinstance(d[key], str) and len(d[key]) == 8 and
                               all(c in "0123456789abcdef" for c in d[key].lower())
                               for d in (da, db) for key in ("raw", "pairswapped", "depth"))
            checks["digest values known"] = hashes_known
            if hashes_known:
                checks["stored(on) == pairswapped(off)"] = db["raw"] == da["pairswapped"]
                checks["stored(off) == pairswapped(on)"] = da["raw"] == db["pairswapped"]
                checks["depth"] = da["depth"] == db["depth"]
                checks["differs from off"] = da["raw"] != db["raw"]
        print("%-12s %s" % (name, " ".join(f"{k}={'ok' if v else 'FAIL'}" for k, v in checks.items())))
        bad += [f"{name}: {k}" for k, v in checks.items() if not v]
    if bad:
        sys.exit("FAILED: " + "; ".join(bad))
    print("byte swap consistent: colour swapped, depth untouched, frames identical")


if __name__ == "__main__":
    main()
