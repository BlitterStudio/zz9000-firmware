#!/usr/bin/env python3
"""Elaborate every build_variant_bitstreams.sh define combination with xvlog/xelab.

Proves that all release variants continue to elaborate clean after
RTL cleanup and constraint changes.
"""
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
VIVADO = Path("D:/Xilinx/Vivado/2018.3/bin")
WORK = HERE / "build" / "variant_elab"
SH_SCRIPT = ROOT / "build_variant_bitstreams.sh"


def parse_variants_and_blocks():
    sh_text = SH_SCRIPT.read_text(encoding="utf-8")
    m = re.search(r"all_variants=\(([^)]+)\)", sh_text)
    if not m:
        raise RuntimeError(f"Could not find all_variants in {SH_SCRIPT}")
    all_vars = m.group(1).split()

    fn = re.search(r"variant_block\(\)\s*\{(.*?)\n\}", sh_text, re.DOTALL)
    if not fn:
        raise RuntimeError(f"Could not find variant_block() in {SH_SCRIPT}")
    cases = re.findall(
        r"([a-zA-Z0-9_\-|]+)\)\s+cat\s+<<\x27EOF\x27\n(.*?)\nEOF",
        fn.group(1),
        re.DOTALL,
    )
    block_map = {}
    for pats, block in cases:
        for p in pats.split("|"):
            block_map[p.strip()] = block + "\n"

    for v in all_vars:
        if v not in block_map:
            raise RuntimeError(f"Variant '{v}' missing define block in {SH_SCRIPT}")

    return all_vars, block_map


def win(p):
    if not isinstance(p, Path):
        return str(p)
    return str(p.resolve()).replace("/", "\\")


def apply_define_block(orig_text, block):
    lines = orig_text.splitlines(True)
    out = []
    skipping = False
    found_start = False
    found_end = False
    for line in lines:
        stripped = line.rstrip("\r\n")
        if stripped == "// ZORRO2/3 switch":
            skipping = True
            found_start = True
            out.append(block)
            continue
        if skipping and stripped == "`define C_S_AXI_DATA_WIDTH 32":
            skipping = False
            found_end = True
            out.append(line)
            continue
        if not skipping:
            out.append(line)
    if not (found_start and found_end):
        raise ValueError("Could not find start/end markers in mntzorro.v")
    return "".join(out)


def relocate_row_bank(text):
    # mntzorro.v declares vc_row_bank after its first use (port connection);
    # simulation tools need it declared first, exactly like the payload
    # contract runner's workaround. Synthesis accepts the original order.
    text = re.sub(r"\s*reg vc_row_bank = 0;", "", text, count=1)
    return re.sub(
        r"(\s*)videocap_sampler #\(",
        r"\1reg vc_row_bank = 0;\1videocap_sampler #(",
        text,
        count=1,
    )


def run(cmd, cwd):
    result = subprocess.run(
        ["cmd", "/c", win(VIVADO / cmd[0]), *map(win, cmd[1:])],
        cwd=cwd,
        text=True,
        capture_output=True,
        timeout=300,
    )
    out = result.stdout + result.stderr
    if result.returncode:
        print(out, file=sys.stderr)
        raise SystemExit(f"{cmd[0]} failed for variant elaboration")
    errors = [l for l in out.splitlines() if "ERROR" in l]
    if errors:
        print("\n".join(errors), file=sys.stderr)
        raise SystemExit(f"{cmd[0]} reported errors")
    return out


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    variants, blocks = parse_variants_and_blocks()
    mntzorro_text = (ROOT / "mntzorro.v").read_text(encoding="utf-8")

    files = [
        ROOT / "videocap_sampler.v",
        ROOT / "videocap_calibration_capture.v",
        ROOT / "videocap_clock_control.v",
        ROOT / "videocap_writeback_layout.v",
        VIVADO.parent / "data/verilog/src/glbl.v",
    ]

    for name in variants:
        simdir = WORK / name
        simdir.mkdir(parents=True, exist_ok=True)
        custom_mntzorro = relocate_row_bank(
            apply_define_block(mntzorro_text, blocks[name])
        )
        (simdir / "mntzorro.v").write_text(custom_mntzorro, encoding="utf-8")
        xvlog_args = ["xvlog"]
        if name == "zorro3-aga":
            xvlog_args.extend(["-d", "VCAP_C28"])
        xvlog_args.extend([simdir / "mntzorro.v", *files])
        run(xvlog_args, simdir)
        run(
            [
                "xelab",
                "-L",
                "xpm",
                "-L",
                "unisims_ver",
                "work.MNTZorro_v0_1_S00_AXI",
                "work.glbl",
                "-s",
                name,
            ],
            simdir,
        )
        print(f"ELAB PASS {name}")
    print("ALL VARIANTS ELABORATE")


if __name__ == "__main__":
    main()
