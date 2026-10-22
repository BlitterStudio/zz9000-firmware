#!/usr/bin/env python3
"""Native xsim proof of the Z3 Fast-RAM autoconfig mapping contract."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
VIVADO = Path(os.environ.get(
    "VIVADO_BIN",
    "D:/Xilinx/Vivado/2018.3/bin" if os.name == "nt" else "/opt/Xilinx/Vivado/2018.3/bin",
))
def native(path):
    if not isinstance(path, Path):
        return str(path)
    path = str(path.resolve())
    return path.replace("/", "\\") if os.name == "nt" else path


def tool(name, *args):
    if os.name == "nt":
        return ["cmd", "/c", native(VIVADO / f"{name}.bat"), *map(native, args)]
    return [str(VIVADO / name), *map(native, args)]


def call(command, cwd):
    result = subprocess.run(command, cwd=cwd, text=True, capture_output=True, timeout=210)
    if result.returncode:
        raise SystemExit(result.stdout + result.stderr)
    return result.stdout + result.stderr


def sim_source():
    text = (ROOT / "mntzorro.v").read_text(encoding="utf-8")
    text = re.sub(r"\s*reg vc_row_bank = 0;", "", text, count=1)
    return re.sub(r"(\s*)videocap_sampler #\(", r"\1reg vc_row_bank = 0;\1videocap_sampler #(", text, count=1)


def main():
    build = HERE / "build"
    build.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="fastram_memmap_", dir=build) as temp:
        work = Path(temp)
        dut = work / "mntzorro.v"
        dut.write_text(sim_source(), encoding="utf-8")
        files = [
            dut,
            ROOT / "videocap_sampler.v",
            ROOT / "videocap_calibration_capture.v",
            ROOT / "videocap_clock_control.v",
            ROOT / "videocap_writeback_layout.v",
            HERE / "fastram_memmap_tb.v",
            VIVADO.parent / "data/verilog/src/glbl.v",
        ]
        call(tool("xvlog", *files), work)
        call(tool("xelab", "-L", "xpm", "-L", "unisims_ver",
                  "work.fastram_memmap_tb", "work.glbl", "-s", "fastram_memmap"), work)
        output = call(tool("xsim", "fastram_memmap", "--runall"), work)
    wanted = "RESULT PASS fastram memory-map contract:"
    if wanted not in output or "RESULT FAIL" in output:
        raise SystemExit(output)
    print(wanted)


if __name__ == "__main__":
    main()
