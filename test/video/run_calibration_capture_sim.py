#!/usr/bin/env python3
"""Exercise raw calibration snapshot capture with independent xsim clocks."""
import os
from pathlib import Path
import subprocess


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def main():
    simdir = HERE / "build" / "sim_calibration_capture"
    simdir.mkdir(parents=True, exist_ok=True)
    default = (
        "D:/Xilinx/Vivado/2018.3/bin"
        if os.name == "nt"
        else "/opt/Xilinx/Vivado/2018.3/bin"
    )
    vivado_bin = Path(os.environ.get("VIVADO_BIN", default))
    suffix = ".bat" if os.name == "nt" else ""

    def run(name, *args):
        result = subprocess.run(
            [str(vivado_bin / (name + suffix)), *map(str, args)],
            cwd=simdir,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=120,
        )
        (simdir / (name + ".log")).write_text(result.stdout, encoding="utf-8")
        if result.returncode:
            print(result.stdout)
            result.check_returncode()
        return result.stdout

    run("xvlog", ROOT / "videocap_calibration_capture.v",
        HERE / "videocap_calibration_capture_tb.v")
    run("xelab", "work.videocap_calibration_capture_tb", "-s", "calibration_capture_tb")
    output = run("xsim", "calibration_capture_tb", "--runall")
    for line in output.splitlines():
        if line.startswith(("CASE ", "MISMATCH", "RESULT ")):
            print(line)
    if "RESULT PASS calibration capture:" not in output or "RESULT FAIL" in output:
        raise SystemExit("Calibration capture regression failed; see " + str(simdir))


if __name__ == "__main__":
    main()
