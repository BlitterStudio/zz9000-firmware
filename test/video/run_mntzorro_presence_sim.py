#!/usr/bin/env python3
"""Prove the MNTZorro RasterIX presence slot without changing legacy slots."""
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
    resolved = str(Path(path).resolve())
    return resolved.replace("/", "\\") if os.name == "nt" else resolved


def tool(name, *args):
    prefix = ["cmd", "/c", native(VIVADO / f"{name}.bat")] if os.name == "nt" else [str(VIVADO / name)]
    return [*prefix, *map(str, args)]


def call(command, cwd):
    result = subprocess.run(command, cwd=cwd, text=True, capture_output=True, timeout=210)
    if result.returncode:
        raise SystemExit(result.stdout + result.stderr)
    return result.stdout + result.stderr

def source():
    text = (ROOT / "mntzorro.v").read_text(encoding="utf-8")
    text = re.sub(r"\s*reg vc_row_bank = 0;", "", text, count=1)
    text = text.replace("  reg [31:0] rxpf_base = 0;    // DDR address of beat 0\n", "", 1)
    text = text.replace("  wire [31:0] rxpf_off = rxpf_rx_addr - rxpf_base;",
                        "  reg [31:0] rxpf_base = 0;    // DDR address of beat 0\n"
                        "  wire [31:0] rxpf_off = rxpf_rx_addr - rxpf_base;", 1)
    return re.sub(r"(\s*)videocap_sampler #\(", r"\1reg vc_row_bank = 0;\1videocap_sampler #(", text, count=1)


def simulate(rasterix):
    build = HERE / "build"
    build.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="mntzorro_presence_", dir=build) as tmp:
        work = Path(tmp)
        (work / "mntzorro.v").write_text(source(), encoding="utf-8")
        files = [
            work / "mntzorro.v",
            ROOT / "videocap_sampler.v",
            ROOT / "videocap_calibration_capture.v",
            ROOT / "videocap_clock_control.v",
            ROOT / "videocap_writeback_layout.v",
            HERE / "mntzorro_presence_tb.v",
            VIVADO.parent / "data/verilog/src/glbl.v",
        ]
        defines = ["-d", "RASTERIX"] if rasterix else []
        call(tool("xvlog", *defines, *map(native, files)), work)
        call(tool("xelab", "-L", "xpm", "-L", "unisims_ver",
                  "work.mntzorro_presence_tb", "work.glbl", "-s", "mntzorro_presence"), work)
        return call(tool("xsim", "mntzorro_presence", "--runall"), work)


def main():
    for rasterix in (False, True):
        output = simulate(rasterix)
        wanted = "RESULT PASS MNTZorro presence contract:"
        if wanted not in output:
            raise SystemExit(output)
        print(("RasterIX" if rasterix else "default"), wanted)


if __name__ == "__main__":
    main()
