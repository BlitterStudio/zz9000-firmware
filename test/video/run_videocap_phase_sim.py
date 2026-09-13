#!/usr/bin/env python3
"""Measure production capture-clock displacement with Vivado UNISIM.

Run with VIVADO_BIN set to the Vivado bin directory if not installed at the
platform default. The wrapper extracts the engine and MMCM verbatim so this
regression cannot silently keep testing a copied, correct clock configuration.
"""
import os
from pathlib import Path
import subprocess


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def main():
    rtl = (ROOT / "mntzorro.v").read_text(encoding="utf-8")
    start = rtl.index("  reg E7M_PSEN = 0;")
    end = rtl.index("  videocap_control_source #(", start)
    wrapper = """`timescale 1ns/1ps
`define ZORRO3
module capture_phase_dut(
    input ZORRO_E7M, input S_AXI_ACLK, input S_AXI_ARESETN,
    input [31:0] axi_reg2, input [31:0] axi_reg3,
    output e7m_shifted, output e7m_shifted180);
wire clkfbout_zz9000_ps_clk_wiz_1_0;
reg video_control_axi_strobe_d = 0;
always @(posedge S_AXI_ACLK)
    video_control_axi_strobe_d <= axi_reg2[31];
""" + rtl[start:end] + "\nendmodule\n"
    simdir = HERE / "build" / "sim_videocap_phase"
    simdir.mkdir(parents=True, exist_ok=True)
    (simdir / "capture_phase_dut.v").write_text(wrapper, encoding="utf-8")
    default = "D:/Xilinx/Vivado/2018.3/bin" if os.name == "nt" else "/opt/Xilinx/Vivado/2018.3/bin"
    tools = Path(os.environ.get("VIVADO_BIN", default))
    suffix = ".bat" if os.name == "nt" else ""

    def run(name, *args):
        result = subprocess.run(
            [str(tools / (name + suffix)), *map(str, args)],
            cwd=simdir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, timeout=120,
        )
        (simdir / (name + ".log")).write_text(result.stdout, encoding="utf-8")
        if result.returncode:
            print(result.stdout)
            result.check_returncode()
        return result.stdout

    run("xvlog", "capture_phase_dut.v", HERE / "videocap_phase_tb.v",
        tools.parent / "data/verilog/src/glbl.v")
    run("xelab", "-L", "unisims_ver", "work.videocap_phase_tb", "work.glbl",
        "-s", "videocap_phase_tb")
    output = run("xsim", "videocap_phase_tb", "--runall")
    for line in output.splitlines():
        if line.startswith(("PHASE ", "MISMATCH", "RESULT ")):
            print(line)
    if "RESULT PASS videocap phase displacement" not in output or "RESULT FAIL" in output:
        raise SystemExit("Capture-clock displacement regression failed; see " + str(simdir))


if __name__ == "__main__":
    main()
