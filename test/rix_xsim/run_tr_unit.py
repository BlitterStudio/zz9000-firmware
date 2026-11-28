#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile and run tb_texture_reader.sv (stand-alone TextureReader test).

usage: run_tr_unit.py --root ROOT --out OUT [--cache BYTES] [--perf]
                      [--seed N] [--sequences N] [--fragments N]

ROOT is the repository tree whose rtl/rasterix is tested; the testbench is
always taken from the tree this script lives in.  Exit status is nonzero on
any mismatch or if the run does not print PASS.
"""
import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

VIVADO = r"D:\Xilinx\Vivado\2018.3"
FILES = [r"3rdParty\sfifo.v", r"3rdParty\skidbuffer.v", r"3rdParty\verilog-axis\axis_broadcast.v",
         r"RasterIX\StreamConcatFifo.v", r"RasterIX\TextureMMU.v",
         r"RasterIX\TextureReaderController.v", r"RasterIX\TextureReaderContext.v",
         r"RasterIX\TextureCacheDirectMapped.v", r"RasterIX\TextureCacheDirectMappedController.v",
         r"RasterIX\TextureCacheDirectMappedContext.v", r"RasterIX\TextureReader.v"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--cache", type=int, default=8192)
    parser.add_argument("--perf", action="store_true")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--sequences", type=int, default=60)
    parser.add_argument("--fragments", type=int, default=400)
    opts = parser.parse_args()

    root = Path(opts.root).resolve()
    out = Path(opts.out).resolve()
    tb = Path(__file__).resolve().parent / "tb_texture_reader.sv"
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    # The XSim-only source adaptation of the tree under test (declaration order).
    rtl = out / "sim_rtl"
    subprocess.run([sys.executable, str(root / "test" / "rix_xsim" / "prepare_xsim_sources.py"),
                    str(root), str(rtl)], check=True, stdout=subprocess.DEVNULL)
    inc = f'-i "{rtl}\\RasterIX" -i "{rtl}\\3rdParty"'
    lines = ["@echo off", f"call {VIVADO}\\settings64.bat", f'cd /d "{out}"']
    for name in FILES:
        lines += [f'call xvlog --relax -sv -d UNITTEST {inc} "{rtl}\\{name}"', "if errorlevel 1 exit /b 1"]
    lines += [f'call xvlog --relax -sv {inc} "{tb}"', "if errorlevel 1 exit /b 1",
              f'call xelab -debug typical -timescale 1ns/1ps -generic_top "CACHE_BYTES={opts.cache}" '
              "-top tb_texture_reader -snapshot tr_sim",
              "if errorlevel 1 exit /b 1"]
    plus = f'--testplusarg "SEED={opts.seed}" --testplusarg "SEQUENCES={opts.sequences}" ' \
           f'--testplusarg "FRAGMENTS={opts.fragments}"'
    if opts.perf:
        plus += ' --testplusarg "PERF=1"'
    lines += [f"call xsim tr_sim -runall {plus}", "exit /b %errorlevel%"]
    bat = out / "run.bat"
    bat.write_text("\n".join(lines) + "\n", encoding="ascii", newline="\r\n")
    with open(out / "run.log", "w", encoding="utf-8", errors="replace") as log:
        rc = subprocess.run(["cmd.exe", "/c", str(bat)], cwd=str(out), stdout=log,
                            stderr=subprocess.STDOUT).returncode
    text = (out / "run.log").read_text(encoding="utf-8", errors="replace")
    for line in text.splitlines():
        if re.match(r"^(ERROR|PERF|TEXTURE_READER|PASS|Fatal|FATAL)", line) or "ERROR:" in line:
            print(line)
    if rc != 0 or "PASS texture reader" not in text:
        sys.exit(f"FAILED rc={rc}, see {out / 'run.log'}")


if __name__ == "__main__":
    main()
