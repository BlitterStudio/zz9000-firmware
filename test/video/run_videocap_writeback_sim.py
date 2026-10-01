#!/usr/bin/env python3
"""Exercise the production M01 AXI FSM under capture loss and backpressure.

The pitch-decode fragment, M01 AXI FSM, declarations, output assignments, AXI
burst constants, and sampler read-bank/address connections are extracted
from mntzorro.v; the wrapper supplies only boundary inputs.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def between(source, start, end):
    if source.count(start) != 1 or source.count(end) != 1:
        raise SystemExit(f"Production extraction marker changed: {start!r} / {end!r}")
    return source[source.index(start):source.index(end, source.index(start))]


def make_wrapper(source):
    geometry = between(source, "  reg [9:0] videocap_y_sync;",
                       "  // Diagnostic target selected")
    declaration = between(source, "  reg [11:0] videocap_pitch;",
                          "  reg [31:0] vcap_probe_data [0:15];")
    pitch_tracker = between(source, "    // OP_DIMENSIONS establishes the writeback pitch.",
                            "    // snoop scanline settings sent over the video-control op path")
    fsm = between(source, "  reg [9:0] videocap_x_sync;",
                  "  // Snapshot the exact WDATA values accepted")
    outputs = between(source, "  assign m01_axi_awaddr  =",
                      "  // AXI DMA defaults")
    constants = between(source, "    m01_axi_awlen <=", "  reg [9:0] videocap_x_sync;")
    # This slice starts inside the production defaults always block and
    # already includes its matching end; retain all actual M01 assignments.
    sampler = between(source, "  videocap_sampler #(", "  xpm_cdc_single #(\n      .DEST_SYNC_FF(3),\n      .INIT_SYNC_FF(1),\n      .SIM_ASSERT_CHK(0),\n      .SRC_INPUT_REG(0)\n  ) videocap_probe_seen_cdc")
    bank = re.search(r"\.buf_rbank\(([^)]+)\)", sampler).group(1)
    address = re.search(r"\.buf_raddr\(([^)]+)\)", sampler).group(1)
    read_address = re.search(r"  wire \[11:0\] vcap_raddr = [^;]+;", source).group(0)
    prefix = """`timescale 1ns/1ps
`define VCAP_FULLRATE_INT 1
module extracted_videocap_writeback (
    input S_AXI_ACLK, input m01_axi_aresetn,
    input vcap_capture_ready_axi, input [11:0] vcap_line_payload_axi,
    input videocap_mode, input [31:0] videocap_address,
    input [7:0] video_control_op, input [31:0] video_control_data,
    input S_AXI_ARESETN, input z_reset,
    input videocap_control_applied_full_width,
    input m01_axi_awready, input m01_axi_wready, input [31:0] vcap_rdata,
    output [31:0] m01_axi_awaddr, output m01_axi_awvalid,
    output [31:0] m01_axi_wdata, output [3:0] m01_axi_wstrb,
    output m01_axi_wvalid, output reg m01_axi_wlast,
    output reg [7:0] m01_axi_awlen, output reg [2:0] m01_axi_awsize,
    output reg [1:0] m01_axi_awburst, output reg [3:0] m01_axi_awcache,
    output reg m01_axi_awlock, output reg [2:0] m01_axi_awprot,
    output reg m01_axi_bready, output [11:0] memory_address,
    output memory_bank, output [3:0] state, output [4:0] beat
);
wire vcap_interlace = 1'b0;
wire [10:0] vcap_y = 0;
wire [10:0] vcap_ymax = 512;
reg video_control_interlace;
"""
    return (prefix + geometry + declaration + read_address + "\n" + outputs +
            "always @(posedge S_AXI_ACLK) begin\n" + constants +
            "always @(posedge S_AXI_ACLK) begin\n" + pitch_tracker + "\n  end\n" + fsm +
            f"assign memory_bank = {bank};\nassign memory_address = {address};\n" +
            "assign state = videocap_save_state;\nassign beat = vc_beat;\nendmodule\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verilator", action="store_true",
                        help="run with Verilator instead of Vivado xsim")
    args = parser.parse_args()
    source = (ROOT / "mntzorro.v").read_text(encoding="utf-8")
    wrapper = make_wrapper(source)
    simdir = HERE / "build" / "sim_videocap_writeback"
    simdir.mkdir(parents=True, exist_ok=True)
    (simdir / "extracted_videocap_writeback.v").write_text(wrapper, encoding="utf-8")
    print("FSM source SHA256 " + hashlib.sha256(source.encode()).hexdigest())
    default = "D:/Xilinx/Vivado/2018.3/bin" if os.name == "nt" else "/opt/Xilinx/Vivado/2018.3/bin"
    vivado_bin = Path(os.environ.get("VIVADO_BIN", default))
    suffix = ".bat" if os.name == "nt" else ""

    def run(command, timeout=120, log=None):
        result = subprocess.run(
            [str(arg) for arg in command], cwd=simdir,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=timeout,
        )
        if log:
            (simdir / log).write_text(result.stdout, encoding="utf-8")
        if result.returncode:
            print(result.stdout)
            result.check_returncode()
        return result.stdout

    def run_vivado(name, *args):
        return run(
            [vivado_bin / (name + suffix), *args],
            log=name + ".log",
        )

    if args.verilator:
        build = simdir / "verilator"
        run(
            ["verilator", "--binary", "--timing", "-Wno-fatal", "-Wno-WIDTH",
             "--top-module", "videocap_writeback_tb", "--Mdir", build,
             simdir / "extracted_videocap_writeback.v",
             ROOT / "videocap_writeback_layout.v",
             HERE / "videocap_writeback_tb.v"],
            timeout=300, log="verilator-build.log",
        )
        output = run([build / "Vvideocap_writeback_tb"])
    else:
        run_vivado("xvlog", "extracted_videocap_writeback.v", ROOT / "videocap_writeback_layout.v",
                   HERE / "videocap_writeback_tb.v")
        run_vivado("xelab", "work.videocap_writeback_tb", "-s", "videocap_writeback_tb")
        output = run_vivado("xsim", "videocap_writeback_tb", "--runall")
    for line in output.splitlines():
        if line.startswith(("CASE ", "MISMATCH ", "RESULT ")):
            print(line)
    if "RESULT PASS writeback:" not in output or "RESULT FAIL" in output:
        raise SystemExit("Capture writeback regression failed; see " + str(simdir))


if __name__ == "__main__":
    main()
