#!/usr/bin/env python3
"""Run source-qualification and fault-injection tests on the clock controller."""
import os
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def main():
    default = "D:/Xilinx/Vivado/2018.3/bin" if os.name == "nt" else "/opt/Xilinx/Vivado/2018.3/bin"
    vivado_bin = Path(os.environ.get("VIVADO_BIN", default))
    suffix = ".bat" if os.name == "nt" else ""

    def run(name, *args):
        result = subprocess.run(
            [str(vivado_bin / (name + suffix)), *map(str, args)],
            cwd=simdir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, timeout=120,
        )
        (simdir / (name + ".log")).write_text(result.stdout, encoding="utf-8")
        if result.returncode:
            print(result.stdout)
            result.check_returncode()
        return result.stdout

    for case, c28_mode in (("c28", 1), ("legacy", 0)):
        simdir = HERE / "build" / "sim_videocap_clock" / case
        simdir.mkdir(parents=True, exist_ok=True)
        (simdir / "clock_case.vh").write_text(
            f"`define MODEL_C28 {c28_mode}\n", encoding="utf-8"
        )
        run("xvlog", "-i", simdir, ROOT / "videocap_clock_control.v",
            HERE / "videocap_clock_control_tb.v")
        run("xelab", "work.videocap_clock_control_tb", "-s", "videocap_clock_control_tb")
        output = run("xsim", "videocap_clock_control_tb", "--runall")
        for line in output.splitlines():
            if line.startswith(("CASE ", "MISMATCH", "RESULT ")):
                print(f"{case}: {line}")
        if "RESULT PASS clock control:" not in output or "RESULT FAIL" in output:
            raise SystemExit("Clock controller regression failed; see " + str(simdir))


if __name__ == "__main__":
    main()
