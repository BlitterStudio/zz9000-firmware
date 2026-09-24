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
    start = rtl.index("  // Capture clock control begins")
    end = rtl.index("  // Capture clock control ends.", start)
    production = rtl[start:end]
    wrapper = """`timescale 1ns/1ps
module capture_phase_dut(
    input ZORRO_E7M, input ZORRO_C28D,
    input S_AXI_ACLK, input S_AXI_ARESETN,
    input [31:0] axi_reg2, input [31:0] axi_reg3,
    output e7m_shifted, output e7m_shifted180);
wire clkfbout_zz9000_ps_clk_wiz_1_0;
reg video_control_axi_strobe_d = 0;
always @(posedge S_AXI_ACLK)
    video_control_axi_strobe_d <= axi_reg2[31];
""" + production + """
// Shorten qualification only; the production clock and phase path is intact.
defparam capture_clock_control.WINDOW_CYCLES = 2000;
endmodule
"""
    default = "D:/Xilinx/Vivado/2018.3/bin" if os.name == "nt" else "/opt/Xilinx/Vivado/2018.3/bin"
    tools = Path(os.environ.get("VIVADO_BIN", default))
    suffix = ".bat" if os.name == "nt" else ""

    def run(simdir, name, *args):
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

    # Legacy checks retain the MMCM's declared model frequency: its physical
    # 7 MHz operating point remains outside specification. C28 cases use
    # representative physical PAL/NTSC frequencies, not a 4x model shortcut.
    for case, c28_mode, period in (
        ("legacy-model", False, "35.0"),
        ("c28-pal", True, "35.24"),
        ("c28-ntsc", True, "34.93"),
    ):
        simdir = HERE / "build" / "sim_videocap_phase" / case
        simdir.mkdir(parents=True, exist_ok=True)
        defines = "`define ZORRO3\n" + ("`define VCAP_C28\n" if c28_mode else "")
        (simdir / "capture_phase_dut.v").write_text(defines + wrapper, encoding="utf-8")
        (simdir / "phase_case.vh").write_text(
            f"`define MODEL_C28 {int(c28_mode)}\n`define MODEL_PERIOD {period}\n",
            encoding="utf-8",
        )
        run(simdir, "xvlog", "-i", simdir, ROOT / "videocap_clock_control.v",
            "capture_phase_dut.v", HERE / "videocap_phase_tb.v",
            tools.parent / "data/verilog/src/glbl.v")
        run(simdir, "xelab", "-L", "unisims_ver", "work.videocap_phase_tb",
            "work.glbl", "-s", "videocap_phase_tb")
        output = run(simdir, "xsim", "videocap_phase_tb", "--runall")
        for line in output.splitlines():
            if line.startswith(("PHASE ", "CASE ", "MISMATCH", "RESULT ")):
                print(f"{case}: {line}")
        if "RESULT PASS videocap phase displacement" not in output or "RESULT FAIL" in output:
            raise SystemExit("Capture-clock displacement regression failed; see " + str(simdir))


if __name__ == "__main__":
    main()
