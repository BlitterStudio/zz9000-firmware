#!/usr/bin/env python3
"""Check the actual restart's IRQ exclusion and ready-state ordering."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
sys.dont_write_bytecode = True
from run_rx_bd import function


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("case", nargs="?", default="all",
                        choices=["all", "success", "failure", "no-base"])
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    firmware = (here.parent.parent / "ZZ9000_proto.sdk/ZZ9000OS/src/ethernet.c").read_text()
    fixture = (here / "rx_restart_test.c").read_text()
    fixture = fixture.replace("/* RESTART_FUNCTION */", function(firmware, "ethernet_restart_dma"))
    with tempfile.TemporaryDirectory(prefix="rx-restart-", dir=here) as build:
        src = Path(build) / "restart.c"; src.write_text(fixture)
        binary = Path(build) / "restart"
        subprocess.run([args.cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                        "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        str(src), "-o", str(binary)], check=True)
        for case in ["success", "failure", "no-base"] if args.case == "all" else [args.case]:
            subprocess.run([str(binary), case], check=True)


if __name__ == "__main__":
    main()
