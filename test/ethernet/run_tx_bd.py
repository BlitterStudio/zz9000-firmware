#!/usr/bin/env python3
"""Run the firmware TX completion handler on the bundled GEM ring routines."""
import argparse
from pathlib import Path
import subprocess
import tempfile

from run_rx_bd import function, macro


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parent.parent
    bsp = root / "ZZ9000_proto.sdk/zz9000_bsp_new/ps7_cortexa9_0/libsrc/emacps_v3_8/src"
    src = root / "ZZ9000_proto.sdk/ZZ9000OS/src"
    firmware = (src / "ethernet.c").read_text()
    ring = (bsp / "xemacps_bdring.c").read_text()
    ring_header = (bsp / "xemacps_bdring.h").read_text()
    bd = (bsp / "xemacps_bd.h").read_text()
    hw = (bsp / "xemacps_hw.h").read_text()
    ring_type = ring_header[ring_header.index("typedef struct {"):ring_header.index("} XEmacPs_BdRing;") + len("} XEmacPs_BdRing;")]
    definitions = [macro((src / "ethernet.h").read_text(), "TXBD_CNT"), ring_type]
    definitions += [macro(hw, name) for name in ["XEMACPS_BD_ADDR_OFFSET", "XEMACPS_BD_STAT_OFFSET",
        "XEMACPS_TXBUF_USED_MASK", "XEMACPS_TXBUF_WRAP_MASK", "XEMACPS_TXBUF_LAST_MASK"]]
    definitions += [macro(ring_header, name) for name in ["XEmacPs_BdRingNext"]]
    definitions += [macro(ring, name) for name in ["XEMACPS_RING_SEEKAHEAD", "XEMACPS_RING_SEEKBACK"]]
    definitions += [macro(bd, name) for name in ["XEmacPs_BdGetStatus", "XEmacPs_BdSetStatus"]]
    ring_functions = "\n\n".join(function(ring, name) for name in ["XEmacPs_BdRingAlloc",
        "XEmacPs_BdRingToHw", "XEmacPs_BdRingFromHwTx", "XEmacPs_BdRingFree"])
    tx_functions = function(firmware, "XEmacPsSendHandler")
    # u32 is unsigned long on the ARM target and unsigned int here; only the
    # error printf's argument type is normalized for the strict host build.
    old_printf = 'printf("XEmacPs_BdRingFree error: %lu\\n",status);'
    assert old_printf in tx_functions
    tx_functions = tx_functions.replace(old_printf, 'printf("XEmacPs_BdRingFree error: %lu\\n",(unsigned long)status);')
    test = (here / "tx_bd_test.c").read_text()
    for marker, value in [("BSP_DEFINITIONS", "\n\n".join(definitions)), ("BSP_FUNCTIONS", ring_functions), ("TX_FUNCTIONS", tx_functions)]:
        assert test.count("/* " + marker + " */") == 1
        test = test.replace("/* " + marker + " */", value)
    with tempfile.TemporaryDirectory(prefix="tx-bd-", dir=here) as build:
        generated = Path(build) / "tx.c"; generated.write_text(test)
        output = Path(build) / "tx"
        subprocess.run([args.cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                        "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        "-I" + str(here), "-I" + str(src), str(generated), "-o", str(output)], check=True)
        subprocess.run([str(output)], check=True)


if __name__ == "__main__":
    main()
