#!/usr/bin/env python3
"""Check that a repeated RX layout command retries a failed restart."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
sys.dont_write_bytecode = True
from run_rx_bd import function

CASES = ["healthy-idempotent", "pre-init", "normal-switch", "enable-retry",
         "disable-retry", "permanent-failure", "failed-startup-default"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("case", nargs="?", default="all", choices=["all"] + CASES)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    firmware = (here.parent.parent / "ZZ9000_proto.sdk/ZZ9000OS/src/ethernet.c").read_text()
    fixture = (here / "rx_offset2_retry_test.c").read_text()
    actual = "\n\n".join(function(firmware, n) for n in ["ethernet_restart_dma", "ethernet_set_rx_offset2"])
    assert fixture.count("/* ACTUAL_FUNCTIONS */") == 1
    fixture = fixture.replace("/* ACTUAL_FUNCTIONS */", actual)
    with tempfile.TemporaryDirectory(prefix="rx-offset2-retry-", dir=here) as build:
        src = Path(build) / "retry.c"; src.write_text(fixture)
        binary = Path(build) / "retry"
        subprocess.run([args.cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                        "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        str(src), "-o", str(binary)], check=True)
        for case in CASES if args.case == "all" else [args.case]:
            subprocess.run([str(binary), case], check=True)


if __name__ == "__main__":
    main()
