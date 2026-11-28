#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the EF testbench once and replay scenes in parallel.

usage: run_tp_scenes.py --root ROOT --scenes SCENES_DIR --out OUT [--latency N]
                        [--jobs N] [--env VAR=value ...] [--tag TAG]
                        [--expect refs/frames.sha256] [scene ...]

ROOT is a repository tree (rtl/, test/rix_xsim/); SCENES_DIR holds the files
written by rix_xsim_gen (<scene>.hex, textures.hex, ...).  The testbench is
compiled into OUT/build once; every scene then runs from its own copy of the
compiled snapshot (parallel xsim runs must not share a snapshot directory).
For each scene the script reports the RESULT cycle count, the TEXPERF line
(texture reader cost) and the SHA-256 of the frame, and writes
OUT/summary-TAG.json.  With --expect, every frame must match the SHA-256 in
the file; refs/frames.sha256 holds the frames of the RTL before the texture
reader lane forwarding, so a change of the texture path has to render
exactly the same frames.  compare_tp_frames.py compares two summaries.

--env takes the run_xsim.bat variables SHARED=1, DMA_BURST=1, RAND_SEED=n,
RAND_LATENCY=n and MEMFETCH=1.
"""
import argparse
import concurrent.futures
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

VIVADO = r"D:\Xilinx\Vivado\2018.3"
INCLUDES = ["RasterIX", r"Float\rtl\float", "3rdParty", r"3rdParty\verilog-axi",
            r"3rdParty\verilog-axis"]
SIM_DIRS = ["3rdParty", r"3rdParty\verilog-axi", r"3rdParty\verilog-axis",
            r"Float\rtl\float", "RasterIX"]


def win(path):
    return str(Path(path).resolve()).replace("/", "\\")


def run_bat(text, directory, name):
    bat = directory / name
    bat.write_text(text, encoding="ascii", newline="\r\n")
    with open(directory / (name + ".log"), "w", encoding="utf-8", errors="replace") as log:
        return subprocess.run(["cmd.exe", "/c", str(bat)], cwd=str(directory), stdout=log,
                              stderr=subprocess.STDOUT).returncode


def compile_build(root, build, tmu):
    sim = build / "sim_rtl"
    if build.exists():
        shutil.rmtree(build)
    build.mkdir(parents=True)
    inc = " ".join(f'-i "{sim}\\{name}"' for name in INCLUDES)
    lines = ["@echo off", f"call {VIVADO}\\settings64.bat", f'cd /d "{build}"',
             f'call py -3 "{root}\\test\\rix_xsim\\prepare_xsim_sources.py" "{root}" "{sim}"',
             "if errorlevel 1 exit /b 1"]
    for sub in SIM_DIRS:
        lines.append(f'for %%F in ("{sim}\\{sub}\\*.v") do call xvlog --relax -sv -d UNITTEST {inc} "%%F"')
        lines.append("if errorlevel 1 exit /b 1")
    variant = "-d RASTERIX_VARIANT_EF" + (" -d RASTERIX_TMU_COUNT_2" if tmu == 2 else "")
    lines += [f'call xvlog --relax -sv -d UNITTEST {variant} {inc} "{root}\\rtl\\zz9k_rix\\zz9k_rasterix.v" '
              f'"{root}\\test\\rix_xsim\\tb_zz9k_rasterix.sv"',
              "if errorlevel 1 exit /b 1",
              'call xvlog --relax "%XILINX_VIVADO%\\data\\verilog\\src\\glbl.v"',
              "if errorlevel 1 exit /b 1",
              "call xelab -debug typical -timescale 1ns/1ps -L xpm -L unisims_ver "
              "-top tb_zz9k_rasterix -top glbl -snapshot tb_zz9k_rasterix_sim",
              "exit /b %errorlevel%"]
    rc = run_bat("\n".join(lines) + "\n", build, "compile.bat")
    if rc != 0 or not (build / "xsim.dir" / "tb_zz9k_rasterix_sim").exists():
        sys.exit(f"compile failed, see {build / 'compile.bat.log'}")


def run_scene(build, out, scenes, scene, latency, envs, tag):
    run = out / f"run-{scene}-{tag}"
    if run.exists():
        shutil.rmtree(run)
    run.mkdir(parents=True)
    shutil.copytree(build / "xsim.dir", run / "xsim.dir")
    sd = win(scenes).replace("\\", "/")
    ppm = win(run).replace("\\", "/") + f"/{scene}.ppm"
    env = dict(item.partition("=")[::2] for item in envs)
    memfetch = env.pop("MEMFETCH", "") == "1"
    command = f"{sd}/{scene}_memfetch.hex" if memfetch else f"{sd}/{scene}.hex"
    args = [f"CMD={command}", f"TEXTURE={sd}/textures.hex", f"PPM={ppm}", f"SCENE={scene}",
            f"LATENCY={latency}", "POLL_LIMIT=6000000", "TRACE_PIPE=0"]
    if memfetch:
        args.append(f"CMD_DDR={sd}/{scene}_memfetch.ddr")
    args += [f"{key}={value}" for key, value in env.items()]
    plus = " ".join(f'--testplusarg "{a}"' for a in args)
    text = "\n".join(["@echo off", f"call {VIVADO}\\settings64.bat", f'cd /d "{run}"',
                      f"xsim tb_zz9k_rasterix_sim -runall {plus}", "exit /b %errorlevel%"]) + "\n"
    rc = run_bat(text, run, "run.bat")
    log = (run / "run.bat.log").read_text(encoding="utf-8", errors="replace")
    result = {"scene": scene, "tag": tag, "rc": rc}
    m = re.search(r"^RESULT scene=\S+ submission=\d+ cycles=(\d+)", log, re.M)
    result["cycles"] = int(m.group(1)) if m else None
    m = re.search(r"^TEXPERF scene=\S+ frags=(\d+) first=(\d+) last=(\d+) span=(\d+) "
                  r"stall=(\d+) fills=(\d+) texels=(\d+)", log, re.M)
    if m:
        keys = ["frags", "first", "last", "span", "stall", "fills", "texels"]
        result.update(dict(zip(keys, map(int, m.groups()))))
    m = re.search(r"^FB_DIGEST scene=\S+ swap=(\d) raw=(\w+) pairswapped=(\w+) depth=(\w+)", log, re.M)
    if m:
        result["digest"] = {"swap": int(m.group(1)), "raw": m.group(2),
                            "pairswapped": m.group(3), "depth": m.group(4)}
    result["pass"] = bool(re.search(r"^PASS scene=", log, re.M))
    result["fatal"] = bool(re.search(r"Fatal|FATAL", log))
    frame = run / f"{scene}.ppm"
    if frame.exists():
        result["sha256"] = hashlib.sha256(frame.read_bytes()).hexdigest()
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--scenes", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--latency", type=int, default=2)
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--tmu", type=int, default=1)
    parser.add_argument("--tag", default="base")
    parser.add_argument("--env", action="append", default=[])
    parser.add_argument("--no-compile", action="store_true")
    parser.add_argument("--expect", help="file of '<sha256>  <scene>' lines the frames must match")
    parser.add_argument("scene", nargs="*")
    opts = parser.parse_args()

    root = win(opts.root)
    out = Path(opts.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    build = out / "build"
    if not opts.no_compile:
        compile_build(root, build, opts.tmu)
    scenes = Path(opts.scenes).resolve()
    names = opts.scene or sorted(p.stem for p in scenes.glob("*.hex")
                                 if "_memfetch" not in p.stem and p.stem != "textures"
                                 and not p.stem.endswith("-tmu2"))
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=opts.jobs) as pool:
        futures = [pool.submit(run_scene, build, out, scenes, n, opts.latency, opts.env, opts.tag)
                   for n in names]
        for future in concurrent.futures.as_completed(futures):
            r = future.result()
            results.append(r)
            print("%-14s rc=%s pass=%s cycles=%s frags=%s span=%s stall=%s fills=%s sha=%s" % (
                r["scene"], r["rc"], r["pass"], r["cycles"], r.get("frags"), r.get("span"),
                r.get("stall"), r.get("fills"), (r.get("sha256") or "-")[:12]), flush=True)
    results.sort(key=lambda r: r["scene"])
    (out / f"summary-{opts.tag}.json").write_text(json.dumps(results, indent=1))
    bad = [r["scene"] for r in results if r["rc"] != 0 or not r["pass"] or r["fatal"]]
    if opts.expect:
        # "<sha256>  <scene>" lines: the frame of every listed scene must match
        # byte for byte (refs/frames.sha256 holds the frames of the pre-change RTL).
        expected = {}
        for line in Path(opts.expect).read_text().splitlines():
            if line.strip():
                digest, name = line.split()
                expected[name] = digest
        for r in results:
            if r["scene"] in expected and r.get("sha256") != expected[r["scene"]]:
                print(f"FRAME MISMATCH {r['scene']}: {r.get('sha256')} != {expected[r['scene']]}")
                bad.append(r["scene"] + " (frame)")
    if bad:
        sys.exit("failed: " + ", ".join(bad))


if __name__ == "__main__":
    main()
