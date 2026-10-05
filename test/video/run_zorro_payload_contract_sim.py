#!/usr/bin/env python3
"""Full MNTZorro Z2/Z3 public-bus payload proof using native Vivado xsim."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
VIVADO = Path(os.environ.get("VIVADO_BIN", "D:/Xilinx/Vivado/2018.3/bin" if os.name == "nt" else "/opt/Xilinx/Vivado/2018.3/bin"))


def native(path):
    if not isinstance(path, Path): return str(path)
    resolved = str(path.resolve())
    return resolved.replace("/", "\\") if os.name == "nt" else resolved

def tool(name, *args):
    head = ["cmd", "/c", native(VIVADO / f"{name}.bat")] if os.name == "nt" else [str(VIVADO / name)]
    return [*head, *map(native, args)]

def call(command, cwd):
    result = subprocess.run(command, cwd=cwd, text=True, capture_output=True, timeout=210)
    if result.returncode: raise SystemExit(result.stdout + result.stderr)
    return result.stdout + result.stderr

def source(z3):
    text = (ROOT / "mntzorro.v").read_text(encoding="utf-8")
    text = re.sub(r"\s*reg vc_row_bank = 0;", "", text, count=1)
    text = re.sub(r"(\s*)videocap_sampler #\(", r"\1reg vc_row_bank = 0;\1videocap_sampler #(", text, count=1)
    if not z3:
        text = text.replace("//`define ZORRO2", "`define ZORRO2", 1)
        text = text.replace("`define ZORRO3", "//`define ZORRO3", 1)
        text = text.replace("`define VARIANT_SUPERDENISE", "//`define VARIANT_SUPERDENISE", 1)
    return text

def simulate(z3):
    build = HERE / "build"
    build.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="zorro_payload_", dir=build) as tmp:
        work = Path(tmp); (work / "mntzorro.v").write_text(source(z3), encoding="utf-8")
        files = [work / "mntzorro.v", ROOT / "videocap_sampler.v", ROOT / "videocap_calibration_capture.v", ROOT / "videocap_clock_control.v", ROOT / "videocap_writeback_layout.v", HERE / "zorro_payload_contract_tb.v", VIVADO.parent / "data/verilog/src/glbl.v"]
        call(tool("xvlog", *files), work)
        call(tool("xelab", "-L", "xpm", "-L", "unisims_ver", "work.zorro_payload_contract_tb", "work.glbl", "-s", "payload"), work)
        return call(tool("xsim", "payload", "--runall"), work)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--z3", action="store_true", help="run only the Zorro III variant")
    args = parser.parse_args()
    variants = (True,) if args.z3 else (False, True)
    for z3 in variants:
        output = simulate(z3)
        wanted = "RESULT PASS zorro payload contract:"
        if wanted not in output:
            raise SystemExit(output)
        print(("Z3" if z3 else "Z2"), wanted)
if __name__ == "__main__": main()
