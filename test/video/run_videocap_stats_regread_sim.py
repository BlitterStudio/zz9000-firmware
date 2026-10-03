#!/usr/bin/env python3
"""Exercise coherent live crop/stats publication and the production read mux."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def production_mux(rtl: str) -> str:
    start = rtl.index("case (regread_addr&'hff)")
    end = rtl.index("              endcase", start) + len("              endcase")
    return rtl[start:end]


def main() -> None:
    mux = production_mux((ROOT / "mntzorro.v").read_text(encoding="utf-8"))
    build_dir = HERE / "build"
    build_dir.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="videocap_stats_", dir=build_dir) as temp:
        build = Path(temp)
        wrapper = build / "videocap_stats_read_mux.v"
        wrapper.write_text(
            "module videocap_stats_read_mux(\n"
            "    input [31:0] regread_addr,\n"
            "    input [9:0] vcap_live_line_count,\n"
            "    output reg [31:0] rr_data\n"
            ");\n"
            "reg [31:0] video_control_vblank = 0;\n"
            "reg [31:0] debug_counter = 0;\n"
            "localparam [15:0] REVISION = 16'hcafe;\n"
            "always @* begin\n" + mux + "\nend\nendmodule\n",
            encoding="utf-8",
        )
        sources = [ROOT / "videocap_sampler.v", HERE / "xpm_cdc_sim.sv",
                   wrapper, HERE / "videocap_live_publish_tb.v"]
        if shutil.which("iverilog") and shutil.which("vvp"):
            executable = build / "live.vvp"
            subprocess.run(["iverilog", "-g2012", "-s", "videocap_live_publish_tb",
                            "-o", str(executable), *map(str, sources)], check=True)
            command = ["vvp", str(executable)]
        elif shutil.which("verilator"):
            mdir = build / "obj_dir"
            subprocess.run(["verilator", "--binary", "--timing", "-Wno-fatal",
                            "--top-module", "videocap_live_publish_tb",
                            "--Mdir", str(mdir), *map(str, sources)], check=True)
            command = [str(mdir / "Vvideocap_live_publish_tb")]
        else:
            def workspace(path: Path) -> str:
                return "/work/" + path.relative_to(ROOT).as_posix()

            executable = workspace(build / "live.vvp")
            command = [
                "docker", "run", "--rm", "--mount",
                f"type=bind,src={ROOT},dst=/work", "debian:bookworm", "sh", "-ec",
                "apt-get update >/dev/null && "
                "apt-get install -y --no-install-recommends iverilog >/dev/null && "
                f"iverilog -g2012 -s videocap_live_publish_tb -o {executable} " +
                " ".join(workspace(path) for path in sources) + f" && vvp {executable}",
            ]
        result = subprocess.run(command, check=True, capture_output=True,
                                text=True, timeout=120)
        print(result.stdout, end="")
        if "RESULT PASS live publication:" not in result.stdout:
            raise SystemExit("Live publication regression did not pass")


if __name__ == "__main__":
    main()
