#!/usr/bin/env python3
"""Exercise the production legacy videocap stats read mux."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
SRC = ROOT / "ZZ9000_proto.sdk" / "ZZ9000OS" / "src"


def production_mux(rtl: str) -> str:
    start = rtl.index("case (regread_addr&'hff)")
    end = rtl.index("              endcase", start) + len("              endcase")
    return rtl[start:end]


def run_local(build: Path, cc: str, verilator: str) -> None:
    subprocess.run(
        [cc, "-I", str(SRC), "-o", str(build / "videocap_stats_capability"),
         str(build / "videocap_stats_capability.c")],
        check=True,
    )
    subprocess.run([str(build / "videocap_stats_capability")], check=True)
    mdir = build / "obj_dir"
    subprocess.run(
        [verilator, "--binary", "--timing", "-Wno-fatal", "--top-module",
         "videocap_stats_regread_tb", "--Mdir", str(mdir),
         str(build / "videocap_stats_regread_tb.v")],
        check=True,
    )
    subprocess.run([str(mdir / "Vvideocap_stats_regread_tb")], check=True)


def run_docker(build: Path) -> None:
    workspace = "/work/" + build.relative_to(ROOT).as_posix()
    subprocess.run(
        [
            "docker", "run", "--rm",
            "--mount", f"type=bind,src={ROOT},dst=/work",
            "debian:bookworm", "sh", "-ec",
            "apt-get update >/dev/null && "
            "apt-get install -y --no-install-recommends build-essential iverilog >/dev/null && "
            "cc -I /work/ZZ9000_proto.sdk/ZZ9000OS/src "
            f"-o {workspace}/videocap_stats_capability "
            f"{workspace}/videocap_stats_capability.c && "
            f"{workspace}/videocap_stats_capability && "
            f"iverilog -g2012 -o {workspace}/videocap_stats_regread.vvp "
            f"{workspace}/videocap_stats_regread_tb.v && "
            f"vvp {workspace}/videocap_stats_regread.vvp",
        ],
        check=True,
    )


def main() -> None:
    rtl = (ROOT / "mntzorro.v").read_text(encoding="utf-8")
    mux = production_mux(rtl)
    cc = shutil.which(os.environ.get("CC", "cc"))
    verilator = shutil.which("verilator")

    build_dir = HERE / "build"
    build_dir.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="videocap_stats_", dir=build_dir) as temp:
        build = Path(temp)
        (build / "videocap_stats_capability.c").write_text(
            "#include \"zz_regs.h\"\n"
            "int main(void) {\n"
            "  return ZZ_FW_CAP_VIDEOCAP_STATS != (1U << 9) ||\n"
            "         !(ZZ_FW_CAPABILITIES & ZZ_FW_CAP_VIDEOCAP_STATS) ||\n"
            "         REG_ZZ_VIDEOCAP_STATS != 0x4E;\n"
            "}\n",
            encoding="utf-8",
        )
        (build / "videocap_stats_regread_tb.v").write_text(
            """`timescale 1ns/1ps
module videocap_stats_read_mux(
    input [31:0] regread_addr,
    input [10:0] vcap_ymax,
    output reg [31:0] rr_data
);
reg [31:0] video_control_vblank = 0;
reg [31:0] debug_counter = 0;
localparam [15:0] REVISION = 16'hcafe;
always @* begin
""" + mux + """
end
endmodule

module videocap_stats_regread_tb;
reg [31:0] regread_addr;
reg [10:0] vcap_ymax;
wire [31:0] rr_data;
videocap_stats_read_mux dut (
    .regread_addr(regread_addr), .vcap_ymax(vcap_ymax), .rr_data(rr_data)
);
initial begin
    regread_addr = 32'h0000004e;
    vcap_ymax = 11'h3ff;
    #1;
    if (rr_data !== 32'h03ff03ff)
        $fatal(1, "0x4e did not return the capped line count in both halves: %h", rr_data);
    vcap_ymax = 11'h5a5;
    #1;
    if (rr_data !== 32'h01a501a5)
        $fatal(1, "0x4e did not reserve bits [15:10] in both halves: %h", rr_data);
    $display("VIDEOCAP STATS REGREAD PASS");
    $finish;
end
endmodule
""",
            encoding="utf-8",
        )
        if cc is not None and verilator is not None:
            run_local(build, cc, verilator)
        else:
            run_docker(build)

    print("VIDEOCAP STATS CAPABILITY PASS")


if __name__ == "__main__":
    main()
