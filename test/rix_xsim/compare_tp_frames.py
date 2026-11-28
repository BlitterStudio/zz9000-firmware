#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare two run_tp_scenes.py summaries.

usage: compare_tp_frames.py BEFORE.json AFTER.json

Every scene present in both summaries must produce a byte-identical frame
(same SHA-256).  Prints the scene cycles and the texture reader cost
(TEXPERF span and TMU stall cycles per fragment) before and after, and exits
nonzero on any frame difference or missing scene.
"""
import json
import sys


def per_fragment(result, key):
    frags = result.get("frags") or 0
    return result[key] / frags if frags and key in result else None


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    before = {r["scene"]: r for r in json.load(open(sys.argv[1]))}
    after = {r["scene"]: r for r in json.load(open(sys.argv[2]))}
    names = sorted(set(before) | set(after))
    failures = []
    print("%-14s %-9s %10s %10s %7s %7s %7s %7s %7s %7s" % (
        "scene", "frame", "cyc before", "cyc after", "frags", "span/f", "span/f", "stall/f", "stall/f", "fills"))
    print("%-14s %-9s %10s %10s %7s %7s %7s %7s %7s %7s" % (
        "", "", "", "", "", "before", "after", "before", "after", "b->a"))
    for name in names:
        b, a = before.get(name), after.get(name)
        if b is None or a is None:
            failures.append(f"{name}: missing in one summary")
            continue
        same = b.get("sha256") is not None and b.get("sha256") == a.get("sha256")
        if not same:
            failures.append(f"{name}: frame differs")

        def fmt(value):
            return "-" if value is None else f"{value:.3f}"

        print("%-14s %-9s %10s %10s %7s %7s %7s %7s %7s %s" % (
            name, "identical" if same else "DIFFERENT", b.get("cycles"), a.get("cycles"),
            b.get("frags"), fmt(per_fragment(b, "span")), fmt(per_fragment(a, "span")),
            fmt(per_fragment(b, "stall")), fmt(per_fragment(a, "stall")),
            f"{b.get('fills')}->{a.get('fills')}"))
    if failures:
        sys.exit("FAILED: " + "; ".join(failures))
    print("all frames identical")


if __name__ == "__main__":
    main()
