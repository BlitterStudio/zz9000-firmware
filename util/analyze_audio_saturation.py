#!/usr/bin/env python3
"""Analyze ZZ9000AX DAC-saturation bench captures (raw PCM files).

Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
SPDX-License-Identifier: GPL-3.0-or-later

One capture file is one step of the combined-level sweep described in
docs/audio-saturation-ceiling.md: a 1 kHz tone through both mixer legs
at a fixed applied leg pair, looped from the ZZ9000AX line-out RCA back
into the line-in RCA and recorded through the qualified AHI capture
path (48 kHz, signed 16-bit stereo, big-endian on the Amiga side).

The tool reports, per channel: peak (dBFS), DC offset, at-rail sample
and region counts, flat-top (crest-flattening) region counts, a
coherent THD proxy, and the notch residual floor. Given the sweep
levels it then reports the saturation onset, the measured ceiling
(highest clean combined level), and the enforced-boundary value the
flip-after-pass runbook installs (ceiling scaled by the 3/4 headroom
policy).

The analysis is deliberately stdlib-only and scale-invariant: the
loopback attenuation is unknown, so absolute captured level carries no
meaning -- distortion shape does. At-rail samples in a capture mean
the CAPTURE (ADC) path clipped, which is a setup fault (add loopback
attenuation), not DAC saturation; DAC saturation appears as flat-top
crests and harmonic growth at any captured scale.

Usage:
  python3 util/analyze_audio_saturation.py [--rate 48000] [--channels 2]
      [--endian be] [--tone 1000] [--rail 32760] [--thd-pct 0.5]
      [--levels 128,160,192,...] [--json] capture_001.raw [...]

Files are analyzed in the order given; with --levels they are assumed
to be ascending sweep steps. Exit status is 0 whenever the analysis
completes; verdicts live in the output (see --json).
"""

import argparse
import json
import math
import sys
from array import array

FULL_SCALE = 32768.0
INT16_MAX = 32767
INT16_MIN = -32768
FLATTOP_REGION_MIN = 3  # reject one-off low-level quantization/noise plateaus


def load_channels(path, channels, big_endian):
    """Read raw interleaved S16 PCM; returns one sample list per channel."""
    with open(path, "rb") as f:
        data = f.read()
    stride = 2 * channels
    if len(data) % stride:
        data = data[: len(data) - (len(data) % stride)]  # drop partial frame
    if not data:
        raise ValueError("no complete frames in %s" % path)
    a = array("h")
    a.frombytes(data)
    if (sys.byteorder == "little") == big_endian:
        a.byteswap()
    return [list(a[c::channels]) for c in range(channels)]

def estimate_tone(samples, rate):
    """Estimate a strong sine from robust local positive-going periods."""
    target = min(len(samples), rate)
    start = (len(samples) - target) // 2
    segment = samples[start:start + target]
    if len(segment) < 3:
        raise ValueError("too few samples for automatic tone detection")
    mean = sum(segment) / float(len(segment))
    crossings = []
    previous = segment[0] - mean
    for index in range(1, len(segment)):
        current = segment[index] - mean
        if previous <= 0.0 < current:
            span = current - previous
            fraction = (-previous / span) if span else 0.0
            crossings.append((index - 1) + fraction)
        previous = current
    if len(crossings) < 3:
        raise ValueError("tone detection found fewer than two periods")

    periods = [b - a for a, b in zip(crossings, crossings[1:])]
    ordered = sorted(periods)
    middle = len(ordered) // 2
    median = (ordered[middle] if len(ordered) & 1 else
              (ordered[middle - 1] + ordered[middle]) / 2.0)
    inliers = [period for period in periods
               if median * 0.98 <= period <= median * 1.02]
    if len(inliers) < 2:
        raise ValueError("tone detection found fewer than two stable periods")
    return rate * len(inliers) / float(sum(inliers))


def coherent_window(rate, tone, available):
    """Largest window of <= ~1 s holding an integer number of tone periods."""
    target = min(available, rate)
    periods = max(1, int(round(tone * target / float(rate))))
    n = int(round(periods * rate / float(tone)))
    return max(1, min(n, available))


def tone_projection(samples, rate, freq):
    """Coherent amplitude and phase of freq over an integer-period window.

    For a window holding an integer number of periods the DFT bin is
    exact, so the single-bin projection needs no windowing function.
    """
    n = len(samples)
    w = 2.0 * math.pi * freq / float(rate)
    cw, sw = math.cos(w), math.sin(w)
    c, s = 1.0, 0.0  # e^{-jwt} by rotation recurrence
    re = im = 0.0
    for x in samples:
        re += x * c
        im -= x * s
        c, s = c * cw - s * sw, s * cw + c * sw
    amplitude = 2.0 * math.hypot(re, im) / n
    phase = math.atan2(im, re)
    return amplitude, phase, w


def distortion_metrics(samples, rate, tone):
    """THD proxy, notch residual and fundamental level."""
    n = coherent_window(rate, tone, len(samples))
    start = (len(samples) - n) // 2
    seg = samples[start:start + n]
    mean = sum(seg) / float(n)
    seg = [x - mean for x in seg]

    amp, phase, w = tone_projection(seg, rate, tone)
    if amp <= 0.0:
        return 0.0, -99.9, -99.9

    harmonics = []
    for h in range(2, 6):
        freq = tone * h
        if freq >= rate / 2.0:
            break
        ha, hp, _ = tone_projection(seg, rate, freq)
        harmonics.append((ha, hp, h))

    thd_pct = 100.0 * math.sqrt(sum(a * a for a, _, _ in harmonics)) / amp

    # Residual after removing DC, the fundamental and the harmonics:
    # coherent reconstruction needs no iteration.
    w0 = 2.0 * math.pi * tone / float(rate)
    acc = 0.0
    for i, x in enumerate(seg):
        model = amp * math.cos(w0 * i + phase)
        for ha, hp, h in harmonics:
            model += ha * math.cos(h * w0 * i + hp)
        d = x - model
        acc += d * d
    residual_dbfs = 20.0 * math.log10(
        max(math.sqrt(acc / n) / FULL_SCALE, 1e-9))
    fundamental_dbfs = 20.0 * math.log10(max(amp / FULL_SCALE, 1e-9))
    return thd_pct, residual_dbfs, fundamental_dbfs


def run_metrics(samples, rail):
    """Peak, DC, at-rail and flat-top counts for one channel."""
    n = len(samples)
    peak = 0
    for x in samples:
        a = -x if x < 0 else x
        if a > peak:
            peak = a
    dc = sum(samples) / float(n)

    rail_samples = 0
    rail_regions = 0
    run = 0
    for x in samples:
        if x >= rail or x <= -rail:
            rail_samples += 1
            run += 1
        else:
            if run:
                rail_regions += 1
            run = 0
    if run:
        rail_regions += 1

    # Flat-top detection is scale-invariant: a clean sine never keeps
    # three consecutive samples within ~0.4% of its peak (adjacent
    # samples near the crest sit ~0.86% apart at 1 kHz/48 kHz), while a
    # clipped or hard-saturated crest parks consecutive samples on the
    # same extreme value. Runs must stay sign-consistent.
    flattop_regions = 0
    flattop_longest = 0
    if peak > 0:
        eps = max(3, peak // 256)
        thresh = peak - eps
        run = 0
        sign = 0
        for x in samples:
            if x >= thresh or x <= -thresh:
                s = 1 if x >= 0 else -1
                if run == 0 or s != sign:
                    if run >= 3:
                        flattop_regions += 1
                    run = 1
                    sign = s
                else:
                    run += 1
                if run > flattop_longest:
                    flattop_longest = run
            else:
                if run >= 3:
                    flattop_regions += 1
                run = 0
        if run >= 3:
            flattop_regions += 1

    return {
        "peak": peak,
        "peak_dbfs": 20.0 * math.log10(max(peak, 1) / FULL_SCALE),
        "dc": dc,
        "rail_samples": rail_samples,
        "rail_regions": rail_regions,
        "flattop_regions": flattop_regions,
        "flattop_longest": flattop_longest,
    }


def analyze_file(path, rate, channels, big_endian, tone, rail, thd_pct,
                 auto_tone=False, selected_channel=None,
                 reference_channel=None):
    loaded = load_channels(path, channels, big_endian)
    if selected_channel is not None:
        if selected_channel < 1 or selected_channel > channels:
            raise ValueError("selected channel outside capture")
        selected = [(selected_channel, loaded[selected_channel - 1])]
    else:
        selected = list(enumerate(loaded, 1))
    if reference_channel is not None:
        if selected_channel is None:
            raise ValueError("reference channel requires selected channel")
        if reference_channel < 1 or reference_channel > channels:
            raise ValueError("reference channel outside capture")
        if reference_channel == selected_channel:
            raise ValueError("reference channel must differ from selected channel")
        reference_samples = loaded[reference_channel - 1]
    else:
        reference_samples = None
    if auto_tone:
        if reference_samples is not None:
            tone_source = reference_samples
        else:
            tone_source = max(
                (samples for _, samples in selected),
                key=lambda samples: max(abs(x) for x in samples))
        measured_tone = estimate_tone(tone_source, rate)
    else:
        measured_tone = tone
    reports = []
    for channel_number, samples in selected:
        m = run_metrics(samples, rail)
        m["channel"] = channel_number
        (m["thd_pct"], m["residual_dbfs"],
         m["fundamental_dbfs"]) = distortion_metrics(
             samples, rate, measured_tone)
        reports.append(m)

    reference = None
    if reference_samples is not None:
        reference = run_metrics(reference_samples, rail)
        reference["channel"] = reference_channel
        (reference["thd_pct"], reference["residual_dbfs"],
         reference["fundamental_dbfs"]) = distortion_metrics(
             reference_samples, rate, measured_tone)
        for m in reports:
            m["thd_rise_pct"] = m["thd_pct"] - reference["thd_pct"]
    fault_reports = reports + ([reference] if reference is not None else [])
    if any(m["rail_regions"] for m in fault_reports):
        verdict = "FAULT"
    elif any(m["flattop_regions"] >= FLATTOP_REGION_MIN
             for m in reports) or \
            any((m["thd_rise_pct"] if reference is not None
                 else m["thd_pct"]) > thd_pct for m in reports):
        verdict = "SATURATED"
    else:
        verdict = "CLEAN"

    return {
        "file": path,
        "frames": len(selected[0][1]),
        "seconds": len(selected[0][1]) / float(rate),
        "tone_hz": measured_tone,
        "channels": reports,
        "reference": reference,
        "verdict": verdict,
    }



HEADROOM = 0.75  # enforced boundary = ceiling * 3/4 (~2.5 dB), see the doc


def find_onset(reports, levels):
    """First non-clean step, the last clean level, and the suggested bound."""
    onset_idx = None
    for i, r in enumerate(reports):
        if r["verdict"] != "CLEAN":
            onset_idx = i
            break
    result = {"onset_index": onset_idx}
    if onset_idx is None:
        result["note"] = ("no saturation onset inside the sweep; extend "
                          "the sweep upward")
        return result
    if onset_idx == 0:
        result["note"] = ("onset at or below the first sweep level; extend "
                          "the sweep downward")
        return result
    ceiling = levels[onset_idx - 1]
    result["ceiling"] = ceiling
    result["onset_level"] = levels[onset_idx]
    result["interval"] = "(%.1f, %.1f]" % (ceiling, levels[onset_idx])
    result["suggested_boundary"] = round(ceiling * HEADROOM, 1)
    return result


def channel_summary(m):
    result = ("peak %.6s dBFS  fundamental %.6s dBFS  dc %+.1f  "
              "thd %.3f%%  flattop %d (max %d)  rail %d/%d" % (
                  "%.2f" % m["peak_dbfs"],
                  "%.2f" % m["fundamental_dbfs"],
                  m["dc"], m["thd_pct"],
                  m["flattop_regions"], m["flattop_longest"],
                  m["rail_samples"], m["rail_regions"]))
    if "thd_rise_pct" in m:
        result += "  thd-rise %+.3f%%" % m["thd_rise_pct"]
    return result


def main(argv=None):
    p = argparse.ArgumentParser(
        description="Report DAC-saturation onset metrics for ZZ9000AX "
                    "bench captures (see docs/audio-saturation-ceiling.md).")
    p.add_argument("files", nargs="+", metavar="FILE",
                   help="raw S16 capture files, ascending sweep order")
    p.add_argument("--rate", type=int, default=48000)
    p.add_argument("--channels", type=int, default=2)
    p.add_argument("--channel", type=int,
                   help="analyze one 1-based capture channel (default all)")
    p.add_argument("--reference-channel", type=int,
                   help="subtract this simultaneous channel's THD floor; "
                        "requires --channel")
    p.add_argument("--endian", choices=("be", "le"), default="be",
                   help="capture byte order (default be: Amiga AHI dump)")
    p.add_argument("--tone", type=float, default=1000.0,
                   help="stimulus tone in Hz (default 1000)")
    p.add_argument("--auto-tone", action="store_true",
                   help="estimate each capture's tone from zero crossings")
    p.add_argument("--rail", type=int, default=32760,
                   help="at-rail threshold, |sample| >= rail (default 32760)")
    p.add_argument("--thd-pct", type=float, default=0.5,
                   help="THD-proxy saturation threshold in %% "
                        "(default 0.5)")
    p.add_argument("--levels", metavar="L1,L2,...",
                   help="combined level per file, ascending; enables the "
                        "onset report")
    p.add_argument("--json", action="store_true",
                   help="machine-readable output")
    args = p.parse_args(argv)
    if args.channel is not None and not 1 <= args.channel <= args.channels:
        p.error("--channel must be between 1 and --channels")
    if args.reference_channel is not None:
        if args.channel is None:
            p.error("--reference-channel requires --channel")
        if not 1 <= args.reference_channel <= args.channels:
            p.error("--reference-channel must be between 1 and --channels")
        if args.reference_channel == args.channel:
            p.error("--reference-channel must differ from --channel")

    levels = None
    if args.levels is not None:
        try:
            levels = [float(x) for x in args.levels.split(",") if x != ""]
        except ValueError:
            p.error("--levels must be comma-separated numbers")
        if len(levels) != len(args.files):
            p.error("--levels needs one value per file (%d given, %d files)"
                    % (len(levels), len(args.files)))

    reports = [analyze_file(f, args.rate, args.channels,
                            args.endian == "be", args.tone,
                            args.rail, args.thd_pct, args.auto_tone,
                            args.channel, args.reference_channel)
               for f in args.files]

    out = {"rate": args.rate,
           "tone": "auto" if args.auto_tone else args.tone,
           "selected_channel": args.channel,
           "reference_channel": args.reference_channel,
           "thd_threshold_pct": args.thd_pct,
           "flattop_region_min": FLATTOP_REGION_MIN,
           "captures": reports}
    if levels is not None:
        out["onset"] = find_onset(reports, levels)

    if args.json:
        print(json.dumps(out, indent=2))
        return 0

    for i, r in enumerate(reports):
        print("%s  %.2f s" % (r["file"], r["seconds"]))
        if args.auto_tone:
            print("  detected tone: %.3f Hz" % r["tone_hz"])
        for m in r["channels"]:
            print("  ch%d: %s" % (m["channel"], channel_summary(m)))
        if r["reference"] is not None:
            m = r["reference"]
            print("  reference ch%d: %s" %
                  (m["channel"], channel_summary(m)))
        print("  verdict: %s" % r["verdict"])
    if levels is not None:
        o = out["onset"]
        if o["onset_index"] is None:
            print("onset: %s" % o["note"])
        elif "ceiling" not in o:
            print("onset: %s (first step %s, level %.1f)" %
                  (o["note"], reports[0]["file"], levels[0]))
        else:
            print("onset: first non-clean step %s (level %.1f)" %
                  (reports[o["onset_index"]]["file"], o["onset_level"]))
            print("measured ceiling (last clean combined level): %.1f  "
                  "true onset in %s" % (o["ceiling"], o["interval"]))
            print("headroom 3/4: AUDIO_SCENE_ENFORCED_BOUNDARY  %.1f" %
                  o["suggested_boundary"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
