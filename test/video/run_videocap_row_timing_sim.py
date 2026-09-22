#!/usr/bin/env python3
"""Prove row metadata and capture distinguish HSYNC from RGB movement."""
import os
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def pattern(pen):
    r = (pen * 197 + 101) & 0xFF
    g = (((r << 3) | (r >> 5)) & 0xFF) ^ 0x5A
    b = (((r << 5) | (r >> 3)) & 0xFF) ^ 0xA5
    return (r << 16) | (g << 8) | b


def load_case(path):
    pixels, metadata = [], []
    for line in path.read_text(encoding="ascii").splitlines():
        kind, value = line.split()
        (pixels if kind == "P" else metadata).append(int(value, 16))
    assert len(pixels) == 1024, (path, len(pixels))
    assert len(metadata) == 12, (path, len(metadata))
    return {"pixels": pixels, "metadata": metadata}


def origins(case):
    result = []
    for row_index in range(4):
        row = case["pixels"][row_index * 256:(row_index + 1) * 256]
        scores = [sum(value != pattern(origin + x)
                      for x, value in enumerate(row))
                  for origin in range(256)]
        best = min(scores)
        candidates = [origin for origin, score in enumerate(scores)
                      if score == best]
        assert len(candidates) == 1
        result.append((candidates[0], best,
                       sum((value ^ pattern(candidates[0] + x)).bit_count()
                           for x, value in enumerate(row))))
    return result


def rows(case):
    words = case["metadata"]
    return [dict(identity=words[row * 3], timing=words[row * 3 + 1],
                 context=words[row * 3 + 2]) for row in range(4)]


def main():
    simdir = HERE / "build" / "sim_videocap_row_timing"
    simdir.mkdir(parents=True, exist_ok=True)
    default = ("D:/Xilinx/Vivado/2018.3/bin" if os.name == "nt"
               else "/opt/Xilinx/Vivado/2018.3/bin")
    vivado_bin = Path(os.environ.get("VIVADO_BIN", default))
    suffix = ".bat" if os.name == "nt" else ""

    def run(name, *args):
        result = subprocess.run(
            [str(vivado_bin / (name + suffix)), *map(str, args)],
            cwd=simdir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, timeout=300,
        )
        (simdir / (name + ".log")).write_text(result.stdout, encoding="utf-8")
        if result.returncode:
            print(result.stdout)
            result.check_returncode()
        return result.stdout

    run("xvlog", ROOT / "videocap_sampler.v",
        ROOT / "videocap_calibration_capture.v",
        HERE / "videocap_row_timing_tb.v",
        vivado_bin.parent / "data/verilog/src/glbl.v")
    run("xelab", "-L", "xpm", "work.videocap_row_timing_tb", "work.glbl",
        "-s", "videocap_row_timing_tb")
    output = run("xsim", "videocap_row_timing_tb", "--runall")
    for line in output.splitlines():
        if line.startswith(("CASE ", "PROGRESS ", "MISMATCH ", "RESULT ")):
            print(line)
    if "RESULT PASS row timing:" not in output or "RESULT FAIL" in output:
        raise SystemExit("Row-timing RTL regression failed; see " + str(simdir))

    names = ["baseline", "alternating_hsync", "alternating_rgb",
             "compensated", "rgb_bit", "real_packet"]
    cases = {name: load_case(simdir / (name + ".txt")) for name in names}
    expected_origins = {
        "baseline": [128, 128, 128, 128],
        "alternating_hsync": [128, 128, 128, 128],
        "alternating_rgb": [128, 129, 128, 129],
        "compensated": [128, 127, 128, 127],
        "rgb_bit": [128, 128, 128, 128],
    }
    observed_origins = {}
    for name, case in cases.items():
        observed = origins(case)
        observed_origins[name] = observed
        if name != "real_packet":
            assert [row[0] for row in observed] == expected_origins[name], (name, observed)
        expected_errors = 1 if name == "rgb_bit" else 0
        assert sum(row[1] for row in observed) == expected_errors, (name, observed)
        assert sum(row[2] for row in observed) == expected_errors, (name, observed)

    assert cases["alternating_hsync"]["pixels"] == cases["baseline"]["pixels"]
    assert cases["alternating_hsync"]["pixels"] != cases["alternating_rgb"]["pixels"]
    assert cases["baseline"]["pixels"] != cases["compensated"]["pixels"]

    decoded = {name: rows(case) for name, case in cases.items()}
    for name, row_set in decoded.items():
        for index, row in enumerate(row_set):
            assert row["identity"] & 0xC0000000 == 0xC0000000, (name, index, row)
            assert (row["identity"] >> 16) & 0x7FF == 104 + index, (name, index, row)

    stable = decoded["baseline"]
    rgb = decoded["alternating_rgb"]
    hsync = decoded["alternating_hsync"]
    compensated = decoded["compensated"]
    for row_set in (stable, rgb):
        assert [row["timing"] & 0xFFFF for row in row_set] == [1820] * 4
        assert [(row["context"] >> 20) & 0xFFF for row in row_set] == [1819] * 4
        assert [(row["context"] >> 8) & 0xFFF for row in row_set] == [1819] * 4

    hsync_intervals = [row["timing"] & 0xFFFF for row in hsync]
    hsync_sample_x = [(row["context"] >> 20) & 0xFFF for row in hsync]
    hsync_phase_x = [(row["context"] >> 8) & 0xFFF for row in hsync]
    assert set(hsync_intervals) == {1819, 1821}, hsync_intervals
    assert set(hsync_sample_x) == {1818, 1820}, hsync_sample_x
    assert hsync_phase_x == hsync_sample_x
    assert [row["timing"] & 0xFFFF for row in compensated] == hsync_intervals
    assert [(row["context"] >> 8) & 0xFFFFFF for row in compensated] == [
        (row["context"] >> 8) & 0xFFFFFF for row in hsync]

    # RGB displacement retains stable line timing; HSYNC displacement does not,
    # even though the normalized capture window now rejects the latter.
    assert [(row["timing"] & 0xFFFF,
             (row["context"] >> 8) & 0xFFFFFF) for row in hsync] != [
        (row["timing"] & 0xFFFF,
         (row["context"] >> 8) & 0xFFFFFF) for row in rgb]

    real = decoded["real_packet"]
    assert [row["timing"] & 0xFFFF for row in real] == [1821, 1819, 1821, 1820]
    assert [(row["identity"] >> 8) & 0xFF for row in real] == [130, 129, 130, 130]
    assert [(row["identity"] >> 27) & 0x3 for row in real] == [0, 3, 0, 0]
    assert [(row["context"] >> 20) & 0xFFF for row in real] == [1820, 1818, 1820, 1819]
    assert [(row["context"] >> 8) & 0xFFF for row in real] == [1820, 1818, 1820, 1819]
    real_origins = [row[0] for row in observed_origins["real_packet"]]
    assert real_origins == [141, 141, 141, 141], (
        "real packet horizontal origin followed accepted HSYNC", real_origins)
    print("RESULT PASS row metadata distinguishes HSYNC and RGB faults")
    print("RESULT PASS real packet retains one horizontal origin")


if __name__ == "__main__":
    main()
