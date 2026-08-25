#!/usr/bin/env python3
# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Self-test for analyze_audio_saturation.py: synthesizes raw S16
# captures (clean tone, interior flat-topped crest, at-rail fault,
# harmonic-distorted tone) and checks the analyzer's metrics, verdicts,
# onset report, endian handling, and JSON mode against hand-computed
# expectations.
import json
import math
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze_audio_saturation as ana


RATE = 48000
TONE = 1000.0


def synth(path, channels, seconds, big_endian, gen):
    """Write raw S16 PCM; gen(i) returns a tuple of channel floats."""
    frames = int(RATE * seconds)
    blob = bytearray()
    for i in range(frames):
        for c in range(channels):
            v = int(round(gen(i, c)))
            v = max(-32768, min(32767, v))
            blob += struct.pack(">h" if big_endian else "<h", v)
    with open(path, "wb") as f:
        f.write(bytes(blob))


def sine(i, amp, freq=TONE):
    return amp * math.sin(2.0 * math.pi * freq * i / RATE)


def clipped_sine(limit):
    def gen(i, c):
        return max(-limit, min(limit, sine(i, 1.25 * 32767.0)))
    return gen


def main():
    tmp = tempfile.mkdtemp(prefix="sat_")

    def path(name):
        return os.path.join(tmp, name)

    # --- clean tone at -6 dBFS: CLEAN, no flat-top, tiny THD ---
    clean = path("clean.raw")
    synth(clean, 2, 1.0, True, lambda i, c: sine(i, 0.5 * 32767.0))
    r = ana.analyze_file(clean, RATE, 2, True, TONE, 32760, 0.5)
    assert r["verdict"] == "CLEAN", r["verdict"]
    for m in r["channels"]:
        assert abs(m["peak_dbfs"] - (-6.02)) < 0.05, m["peak_dbfs"]
        assert abs(m["fundamental_dbfs"] - (-6.02)) < 0.05, \
            m["fundamental_dbfs"]
        assert m["rail_samples"] == 0 and m["rail_regions"] == 0
        assert m["flattop_regions"] == 0 and m["flattop_longest"] <= 1, m
        assert m["thd_pct"] < 0.05, m["thd_pct"]
        assert abs(m["dc"]) < 1.0, m["dc"]

    # One isolated three-sample plateau can come from low-level
    # quantization/noise; it is diagnostic, not a saturation verdict.
    isolated = path("isolated-flattop.raw")
    crest = 0.5 * 32767.0
    synth(isolated, 2, 1.0, True,
          lambda i, c: crest if i in (11, 12, 13)
          else sine(i, crest))
    r = ana.analyze_file(isolated, RATE, 2, True, TONE, 32760, 0.5)
    assert r["verdict"] == "CLEAN", r
    assert all(m["flattop_regions"] == 1 for m in r["channels"]), r

    # --- Paula-clock tone: auto detection preserves coherent THD ---
    paula_tone = path("paula-tone.raw")
    actual_paula_tone = 998.75
    synth(paula_tone, 2, 1.0, True,
          lambda i, c: sine(i, 0.5 * 32767.0, actual_paula_tone))
    r = ana.analyze_file(paula_tone, RATE, 2, True, TONE, 32760, 0.5,
                         auto_tone=True)
    assert abs(r["tone_hz"] - actual_paula_tone) < 0.05, r["tone_hz"]
    assert r["verdict"] == "CLEAN", r["verdict"]
    for m in r["channels"]:
        assert m["thd_pct"] < 0.05, m["thd_pct"]

    # Local period filtering rejects isolated spurious crossings rather
    # than biasing the estimate across the complete capture.
    crossed = [round(0.5 * 32767.0 * math.sin(
        2.0 * math.pi * actual_paula_tone * i / RATE))
        for i in range(RATE)]
    for start in (12000, 24000, 36000):
        crossed[start:start + 4] = [-1000, 1000, -1000, 1000]
    assert abs(ana.estimate_tone(crossed, RATE) - actual_paula_tone) < 0.05

    # Auto detection follows the strongest capture channel; a Paula
    # player may place its mono sample on either side.
    paula_right = path("paula-right.raw")
    synth(paula_right, 2, 1.0, True,
          lambda i, c: 0 if c == 0 else
          sine(i, 0.5 * 32767.0, actual_paula_tone))
    r = ana.analyze_file(paula_right, RATE, 2, True, TONE, 32760, 0.5,
                         auto_tone=True)
    assert abs(r["tone_hz"] - actual_paula_tone) < 0.05, r["tone_hz"]
    r = ana.analyze_file(paula_right, RATE, 2, True, TONE, 32760, 0.5,
                         auto_tone=True, selected_channel=2)
    assert len(r["channels"]) == 1, r
    assert r["channels"][0]["channel"] == 2, r
    assert r["verdict"] == "CLEAN", r

    # A simultaneous reference channel establishes a non-zero source
    # distortion floor; only THD growth above that floor drives onset.
    referenced_clean = path("referenced-clean.raw")
    referenced_dirty = path("referenced-dirty.raw")
    reference_h3 = 0.005 * 32767.0  # 1.0% against the 0.5-FS reference
    synth(referenced_clean, 2, 1.0, True,
          lambda i, c: sine(i, (0.3 if c == 0 else 0.5) * 32767.0)
          + (0.0042 * 32767.0 if c == 0 else reference_h3)
          * math.sin(2.0 * math.pi * 3 * TONE * i / RATE))
    synth(referenced_dirty, 2, 1.0, True,
          lambda i, c: sine(i, (0.3 if c == 0 else 0.5) * 32767.0)
          + (0.0048 * 32767.0 if c == 0 else reference_h3)
          * math.sin(2.0 * math.pi * 3 * TONE * i / RATE))
    r_clean = ana.analyze_file(
        referenced_clean, RATE, 2, True, TONE, 32760, 0.5,
        selected_channel=1, reference_channel=2)
    r_dirty = ana.analyze_file(
        referenced_dirty, RATE, 2, True, TONE, 32760, 0.5,
        selected_channel=1, reference_channel=2)
    assert r_clean["verdict"] == "CLEAN", r_clean
    assert r_dirty["verdict"] == "SATURATED", r_dirty
    assert r_dirty["reference"]["channel"] == 2, r_dirty
    assert abs(r_dirty["reference"]["thd_pct"] - 1.0) < 0.05, r_dirty
    assert abs(r_dirty["channels"][0]["thd_rise_pct"] - 0.6) < 0.05, \
        r_dirty
    o = ana.find_onset([r_clean, r_dirty], [48.0, 56.0])
    assert o["interval"] == "(48.0, 56.0]", o

    # --- DAC saturation as seen through loopback attenuation: crest
    # flattened at an interior level (not the capture rail) ---
    saturated = path("saturated.raw")
    synth(saturated, 2, 1.0, True, clipped_sine(0.4 * 32767.0))
    r = ana.analyze_file(saturated, RATE, 2, True, TONE, 32760, 0.5)
    assert r["verdict"] == "SATURATED", r["verdict"]
    for m in r["channels"]:
        assert m["rail_samples"] == 0, "interior clipping must not hit rail"
        assert m["flattop_regions"] > 0 and m["flattop_longest"] >= 3, m
        assert m["thd_pct"] > 0.5, m["thd_pct"]

    # --- capture-path fault: samples parked at the int16 rail ---
    fault = path("fault.raw")
    synth(fault, 2, 1.0, True, clipped_sine(32767.0))
    r = ana.analyze_file(fault, RATE, 2, True, TONE, 32760, 0.5)
    assert r["verdict"] == "FAULT", r["verdict"]
    for m in r["channels"]:
        assert m["rail_samples"] > 0 and m["rail_regions"] > 0, m
        assert m["rail_samples"] > m["rail_regions"], \
            "regions must group consecutive at-rail samples"

    # --- harmonic distortion without flat-top: THD-only verdict ---
    # 5% H3 steepens the crest but keeps it curved (verified: the
    # longest in-band run stays 1), so only the THD criterion fires.
    harmonic = path("harmonic.raw")
    synth(harmonic, 2, 1.0, True,
          lambda i, c: sine(i, 0.4 * 32767.0)
          + 0.02 * 32767.0 * math.sin(2.0 * math.pi * 3 * TONE * i / RATE))
    r = ana.analyze_file(harmonic, RATE, 2, True, TONE, 32760, 0.5)
    assert r["verdict"] == "SATURATED", r["verdict"]
    for m in r["channels"]:
        assert m["flattop_regions"] == 0, m
        expected = 100.0 * 0.02 / 0.4
        assert abs(m["thd_pct"] - expected) < 0.1 * expected, \
            (m["thd_pct"], expected)

    # --- onset report over an ascending three-step sweep ---
    mid = path("mid.raw")
    synth(mid, 2, 1.0, True, lambda i, c: sine(i, 0.6 * 32767.0))
    files = [clean, mid, saturated]
    levels = [192.0, 224.0, 256.0]
    reports = [ana.analyze_file(f, RATE, 2, True, TONE, 32760, 0.5)
               for f in files]
    o = ana.find_onset(reports, levels)
    assert o["onset_index"] == 2, o
    assert o["ceiling"] == 224.0 and o["onset_level"] == 256.0, o
    assert o["interval"] == "(224.0, 256.0]", o
    assert o["suggested_boundary"] == 168.0, o
    # all-clean sweep and first-step-already-dirty sweep degrade loudly
    o = ana.find_onset(reports[:2], levels[:2])
    assert o["onset_index"] is None and "extend" in o["note"], o
    o = ana.find_onset(reports[2:], levels[2:])
    assert o["onset_index"] == 0 and "extend" in o["note"], o

    # --- endian: same samples, both byte orders, identical metrics ---
    le = path("le.raw")
    synth(le, 2, 1.0, False, lambda i, c: sine(i, 0.5 * 32767.0))
    r_be = ana.analyze_file(clean, RATE, 2, True, TONE, 32760, 0.5)
    r_le = ana.analyze_file(le, RATE, 2, False, TONE, 32760, 0.5)
    for a, b in zip(r_be["channels"], r_le["channels"]):
        assert a["peak"] == b["peak"], (a["peak"], b["peak"])
        assert abs(a["thd_pct"] - b["thd_pct"]) < 1e-9

    # --- mono capture ---
    mono = path("mono.raw")
    synth(mono, 1, 1.0, True, lambda i, c: sine(i, 0.25 * 32767.0))
    r = ana.analyze_file(mono, RATE, 1, True, TONE, 32760, 0.5)
    assert len(r["channels"]) == 1 and r["verdict"] == "CLEAN", r

    # --- CLI: table mode exits 0; JSON mode round-trips ---
    import contextlib
    import io
    with contextlib.redirect_stdout(io.StringIO()):
        assert ana.main(["--levels", "224,256", mid, saturated]) == 0
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = ana.main(["--json", "--levels", "224,256", mid, saturated])
    assert rc == 0
    doc = json.loads(buf.getvalue())
    assert len(doc["captures"]) == 2, doc
    assert doc["captures"][1]["verdict"] == "SATURATED", doc
    assert doc["onset"]["suggested_boundary"] == 168.0, doc
    assert doc["flattop_region_min"] == 3, doc
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = ana.main(["--json", "--auto-tone", paula_tone])
    assert rc == 0
    doc = json.loads(buf.getvalue())
    assert doc["tone"] == "auto", doc
    assert abs(doc["captures"][0]["tone_hz"] - actual_paula_tone) < 0.05
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = ana.main(["--json", "--auto-tone", "--channel", "2",
                       paula_right])
    assert rc == 0
    doc = json.loads(buf.getvalue())
    assert doc["selected_channel"] == 2, doc
    assert doc["captures"][0]["channels"][0]["channel"] == 2, doc
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = ana.main(["--json", "--channel", "1",
                       "--reference-channel", "2", referenced_dirty])
    assert rc == 0
    doc = json.loads(buf.getvalue())
    assert doc["reference_channel"] == 2, doc
    assert doc["captures"][0]["reference"]["channel"] == 2, doc

    for f in os.listdir(tmp):
        os.unlink(os.path.join(tmp, f))
    os.rmdir(tmp)
    print("PASS")


if __name__ == "__main__":
    main()
