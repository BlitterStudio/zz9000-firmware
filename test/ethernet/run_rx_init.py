#!/usr/bin/env python3
"""Exercise actual GEM ring initialization and failure unwinding on the host."""
import argparse
from pathlib import Path
import subprocess
import re
import tempfile
import sys

sys.dont_write_bytecode = True
from run_rx_bd import function, macro


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    cases = ["success", "rx-create", "rx-clone", "tx-create", "tx-clone",
             "alloc", "prepare", "commit"]
    parser.add_argument("case", nargs="?", choices=["all"] + cases, default="all")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parent.parent
    bsp = root / "ZZ9000_proto.sdk/zz9000_bsp_new/ps7_cortexa9_0/libsrc/emacps_v3_8/src"
    firmware = (root / "ZZ9000_proto.sdk/ZZ9000OS/src/ethernet.c").read_text()
    ring = (bsp / "xemacps_bdring.c").read_text()
    header = (bsp / "xemacps_bdring.h").read_text()
    bd = (bsp / "xemacps_bd.h").read_text()
    hw = re.sub(r"/\*.*?\*/", "", (bsp / "xemacps_hw.h").read_text(), flags=re.S)
    definitions = [header[header.index("typedef struct {"):header.index("} XEmacPs_BdRing;") + len("} XEmacPs_BdRing;")]]
    firmware_header = (root / "ZZ9000_proto.sdk/ZZ9000OS/src/ethernet.h").read_text()
    definitions += [macro(firmware_header, n) for n in ["RXBD_CNT", "TXBD_CNT"]]
    definitions += [macro(hw, n) for n in ["XEMACPS_BD_ADDR_OFFSET", "XEMACPS_BD_STAT_OFFSET",
        "XEMACPS_RXBUF_NEW_MASK", "XEMACPS_RXBUF_WRAP_MASK", "XEMACPS_RXBUF_ADD_MASK",
        "XEMACPS_TXBUF_USED_MASK", "XEMACPS_TXBUF_WRAP_MASK", "XEMACPS_SEND", "XEMACPS_RECV"]]
    definitions += [macro(hw, "XEMACPS_BD_ALIGNMENT", 1), macro(bd, "XEMACPS_DMABD_MINIMUM_ALIGNMENT", 1)]
    definitions += [macro(header, "XEmacPs_BdRingNext")]
    definitions += [macro(ring, n) for n in ["XEMACPS_RING_SEEKAHEAD", "XEMACPS_RING_SEEKBACK"]]
    definitions += [macro(bd, n) for n in ["XEmacPs_BdClear", "XEmacPs_BdClearRxNew", "XEmacPs_BdSetStatus"]]
    definitions += [macro(bd, "XEmacPs_BdSetAddressRx", 1)]
    bsp_functions = "\n\n".join(function(ring, n) for n in ["XEmacPs_BdSetRxWrap", "XEmacPs_BdSetTxWrap",
        "XEmacPs_BdRingCreate", "XEmacPs_BdRingClone", "XEmacPs_BdRingAlloc", "XEmacPs_BdRingUnAlloc", "XEmacPs_BdRingToHw"])
    # Bundled Create has a computed but unused physical-address local. An
    # explicit void-use permits Werror without changing its values or branches.
    bsp_functions = bsp_functions.replace("/* Setup and initialize pointers and counters */", "(void)BdPhyAddr;\n\t/* Setup and initialize pointers and counters */")
    helpers = "\n\n".join(function(firmware, n) for n in ["ethernet_prepare_rx_bd", "ethernet_clear_host_state"])
    init = function(firmware, "init_ethernet_buffers").replace("int init_ethernet_buffers()", "int init_ethernet_buffers(void)")
    test = (here / "rx_init_test.c").read_text()
    for name, content in [("BSP_DEFINITIONS", "\n\n".join(definitions)), ("BSP_FUNCTIONS", bsp_functions),
                          ("FIRMWARE_HELPERS", helpers), ("INIT_FUNCTION", init)]:
        marker = "/* " + name + " */"
        assert test.count(marker) == 1, name
        test = test.replace(marker, content)
    with tempfile.TemporaryDirectory(prefix="rx-init-", dir=here) as build:
        generated = Path(build) / "init.c"
        generated.write_text(test)
        output = Path(build) / "init"
        subprocess.run([args.cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic", "-O1", "-g",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I" + str(here),
                        str(generated), "-o", str(output)], check=True)
        for case in cases if args.case == "all" else [args.case]:
            subprocess.run([str(output), case], check=True)


if __name__ == "__main__":
    main()
