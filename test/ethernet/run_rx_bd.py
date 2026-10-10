#!/usr/bin/env python3
"""Run actual firmware RX routines and bundled GEM ring routines with stub DMA."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, name):
    match = re.search(r"^(?:static )?(?:void|int|LONG|u32) " + name + r"\([^;]*?\n?\{", source, re.M)
    assert match, name
    depth = 0
    tokens = r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]'
    for token in re.finditer(tokens, source[match.end() - 1:], re.S):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[match.start():match.end() - 1 + token.end()]
    raise ValueError("Unclosed function " + name)


def macro(source, name, occurrence=0):
    lines = source.splitlines()
    starts = [i for i, line in enumerate(lines) if re.match(r"#define " + name + r"(?:\(|\s)", line)]
    start = end = starts[occurrence]
    while lines[end].endswith("\\"):
        end += 1
    return "\n".join(lines[start:end + 1])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    cases = ["pressure", "offset2", "scan", "publish", "rollback", "cycle", "burst"]
    parser.add_argument("case", nargs="?", default="all", choices=["all"] + cases)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parent.parent
    bsp = root / "ZZ9000_proto.sdk/zz9000_bsp_new/ps7_cortexa9_0/libsrc/emacps_v3_8/src"
    firmware = (root / "ZZ9000_proto.sdk/ZZ9000OS/src/ethernet.c").read_text()
    ring = (bsp / "xemacps_bdring.c").read_text()
    ring_header = (bsp / "xemacps_bdring.h").read_text()
    bd = (bsp / "xemacps_bd.h").read_text()
    hw = (bsp / "xemacps_hw.h").read_text()
    ring_type = ring_header[ring_header.index("typedef struct {"):ring_header.index("} XEmacPs_BdRing;") + len("} XEmacPs_BdRing;")]
    src = root / "ZZ9000_proto.sdk/ZZ9000OS/src"
    eth_h = (src / "ethernet.h").read_text()
    memmap = (src / "memorymap.h").read_text()
    definitions = [macro(eth_h, n) for n in ["FRAME_MAX_BACKLOG", "RXBD_CNT", "ETH_RX_LEN_OFFSET2"]]
    definitions += [macro(memmap, n) for n in ["RX_FRAME_PAD", "FRAME_SIZE"]]
    definitions += [macro(firmware, n) for n in ["ETH_INVALID_BACKLOG_SLOT", "ETH_BACKLOG_HIGH_WATERMARK"]]
    definitions += [ring_type]
    definitions += [macro(hw, name) for name in ["XEMACPS_BD_ADDR_OFFSET", "XEMACPS_BD_STAT_OFFSET",
        "XEMACPS_RXBUF_NEW_MASK", "XEMACPS_RXBUF_WRAP_MASK", "XEMACPS_RXBUF_ADD_MASK",
        "XEMACPS_RXBUF_EOF_MASK", "XEMACPS_RXBUF_LEN_MASK", "XEMACPS_RXBUF_IDMATCH_MASK"]]
    definitions += [macro(ring_header, name) for name in ["XEmacPs_BdRingNext", "XEmacPs_BdRingGetFreeCnt"]]
    definitions += [macro(ring, name) for name in ["XEMACPS_RING_SEEKAHEAD", "XEMACPS_RING_SEEKBACK"]]
    definitions += [macro(bd, name) for name in ["XEmacPs_BdClearRxNew", "XEmacPs_BdIsRxNew", "XEmacPs_BdGetLength"]]
    definitions += [macro(bd, "XEmacPs_BdSetAddressRx", 1)]  # Actual 32-bit ARM variant.
    ring_functions = "\n\n".join(function(ring, name) for name in ["XEmacPs_BdRingAlloc",
        "XEmacPs_BdRingUnAlloc", "XEmacPs_BdRingToHw", "XEmacPs_BdRingFromHwRx", "XEmacPs_BdRingFree"])
    rx_functions = "\n\n".join(function(firmware, name) for name in ["ethernet_prepare_rx_bd",
        "ethernet_unprepare_rx_bd", "ethernet_alloc_rx_frames", "XEmacPsRecvHandler"])
    rx_functions = rx_functions.replace("void ethernet_alloc_rx_frames()", "void ethernet_alloc_rx_frames(void)")
    # BdGetLength is masked to 13 bits. Normalize its local type for the strict
    # host fixture; values and control flow are unchanged. The firmware's own
    # signed/unsigned comparison here is left as it is.
    assert "#define XEMACPS_RXBUF_LEN_MASK       0x00001FFFU" in hw
    rx_functions = rx_functions.replace("int rx_bytes = XEmacPs_BdGetLength", "u32 rx_bytes = XEmacPs_BdGetLength")
    test = (here / "rx_bd_test.c").read_text()
    for marker, value in [("BSP_DEFINITIONS", "\n\n".join(definitions)), ("BSP_FUNCTIONS", ring_functions), ("RX_FUNCTIONS", rx_functions)]:
        assert test.count("/* " + marker + " */") == 1
        test = test.replace("/* " + marker + " */", value)
    with tempfile.TemporaryDirectory(prefix="rx-bd-", dir=here) as build:
        generated = Path(build) / "rx.c"; generated.write_text(test)
        output = Path(build) / "rx"
        subprocess.run([args.cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                        "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        "-I" + str(here), str(generated), "-o", str(output)], check=True)
        for case in cases if args.case == "all" else [args.case]:
            subprocess.run([str(output), case], check=True)


if __name__ == "__main__":
    main()
