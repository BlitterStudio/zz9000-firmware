#!/usr/bin/env python3
"""Keep every bitstream variant's capture configuration honest.

The videocap variant selection lives in mntzorro.v preprocessor ladders,
so no simulator or host C test sees it. Issue #76 shipped an a500plus
build whose RGB_MODE sampled the upper nibble of VCAP_R/G/B - pins the
ZZ9500CX Denise adapter never wires - because the VARIANT_SUPERDENISE
arm of each ladder had drifted from the proven VARIANT_ZZ9500 (a500)
configuration.

This test splices each variant's define block from
build_variant_bitstreams.sh into mntzorro.v exactly the way the build
does, evaluates the ifdef ladders the way Verilog compilation does, and
asserts the resulting sampler configuration. It catches drift between
the Denise-adapter variants before a bitstream is built.
"""

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RTL_PATH = ROOT / "mntzorro.v"
BUILD_SCRIPT = ROOT / "build_variant_bitstreams.sh"

BLOCK_START = "// ZORRO2/3 switch"
BLOCK_END = "`define C_S_AXI_DATA_WIDTH 32"


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def evaluate(rtl: str):
    """Evaluate the preprocessor: `define assignments accumulate while
    scanning, and ifdef ladders test the defines visible at that point.

    Returns (effective_defines, surviving_source).
    """
    defines = {}
    text = []
    stack = []  # [parent_active, branch_taken, active]
    for raw in strip_comments(rtl).splitlines():
        line = raw.strip()
        if line.startswith("`"):
            tokens = line.split(None, 1)
            directive = tokens[0]
            arg = tokens[1].strip() if len(tokens) > 1 else ""
            parent = stack[-1][2] if stack else True
            if directive == "`ifdef":
                cond = arg in defines
                stack.append([parent, cond, parent and cond])
            elif directive == "`ifndef":
                cond = arg not in defines
                stack.append([parent, cond, parent and cond])
            elif directive == "`elsif":
                frame = stack[-1]
                if frame[0] and not frame[1]:
                    cond = arg in defines
                    frame[1] = cond
                    frame[2] = cond
                else:
                    frame[2] = False
            elif directive == "`else":
                frame = stack[-1]
                frame[2] = frame[0] and not frame[1]
            elif directive == "`endif":
                stack.pop()
            elif directive == "`define" and all(f[2] for f in stack):
                parts = arg.split(None, 1)
                defines[parts[0]] = parts[1].strip() if len(parts) > 1 else ""
            continue
        if all(f[2] for f in stack):
            text.append(line)
    return defines, "\n".join(text)


def splice_variant_block(rtl: str, block: str) -> str:
    """Mirror replace_define_block() in build_variant_bitstreams.sh:
    substitute the region from the variant switch through (excluding)
    the C_S_AXI_DATA_WIDTH line."""
    out = []
    skipping = False
    replaced = False
    for line in rtl.splitlines(keepends=True):
        content = line.rstrip("\r\n")
        if content == BLOCK_START:
            out.append(block if block.endswith("\n") else block + "\n")
            skipping = True
            replaced = True
            continue
        if skipping and content == BLOCK_END:
            skipping = False
            out.append(line)
            continue
        if not skipping:
            out.append(line)
    if not replaced or skipping:
        raise SystemExit("variant define block markers not found in mntzorro.v")
    return "".join(out)


def variant_block_from_script(script: str, variant: str) -> str:
    """Extract the define block build_variant_bitstreams.sh installs."""
    match = re.search(
        rf"^\s*{re.escape(variant)}\)\n(.*?)^EOF$", script, flags=re.S | re.M
    )
    if not match:
        raise SystemExit(f"variant block not found in build script: {variant}")
    return match.group(1)


class VariantConfig:
    def __init__(self, name, rtl):
        self.name = name
        self.defines, self.text = evaluate(rtl)

    def vcap(self, key):
        return self.defines.get(f"VCAP_{key}")

    def mmcm(self, param):
        match = re.search(rf"\.{param}\(([\d.]+)\)", self.text)
        return match.group(1) if match else None

    def has(self, fragment):
        normalized = re.sub(r"\s+", " ", fragment)
        return normalized in re.sub(r"\s+", " ", self.text)


def die(config, what, detail):
    raise SystemExit(
        f"capture contract violated for {config.name}: {what} ({detail})"
    )


def check_variant(config, expect):
    """expect: RGB_MODE, CSYNC_VSYNC, FULLRATE, CLKOUT0_DIVIDE_F,
    CLKOUT0_PHASE, CLKOUT1_PHASE, letterbox, and the bus defines the
    variant must set."""
    for key in ("RGB_MODE", "CSYNC_VSYNC", "FULLRATE_INT"):
        want = expect[f"VCAP_{key}"]
        got = config.vcap(key)
        if got != want:
            die(config, f"VCAP_{key}", f"expected {want}, got {got}")
    for param, want in (
        ("CLKOUT0_DIVIDE_F", expect["CLKOUT0_DIVIDE_F"]),
        ("CLKOUT0_PHASE", expect["CLKOUT0_PHASE"]),
        ("CLKOUT1_PHASE", expect["CLKOUT1_PHASE"]),
    ):
        got = config.mmcm(param)
        if got is None or float(got) != float(want):
            die(config, param, f"expected {want}, got {got}")
    letterbox = "videocap_ymax_sync <= (vcap_ymax<<1)-(2*40);"
    if config.has(letterbox) != expect["letterbox"]:
        die(
            config,
            "Denise ymax letterbox",
            f"expected present={expect['letterbox']}",
        )
    for required in expect.get("bus_defines", ()):
        if required not in config.defines:
            die(config, "variant defines", f"missing {required}")


VIDEO_SLOT = {
    "VCAP_RGB_MODE": "0",
    "VCAP_CSYNC_VSYNC": "0",
    "VCAP_FULLRATE_INT": "1",
    "CLKOUT0_DIVIDE_F": "8",
    "CLKOUT0_PHASE": "90",
    "CLKOUT1_PHASE": "0",
    "letterbox": False,
}

ZORRO2_SLOT = {
    "VCAP_RGB_MODE": "2",
    "VCAP_CSYNC_VSYNC": "0",
    "VCAP_FULLRATE_INT": "1",
    "CLKOUT0_DIVIDE_F": "8",
    "CLKOUT0_PHASE": "270",
    "CLKOUT1_PHASE": "135",
    "letterbox": False,
}

# a500 (OCS Denise) and a500plus (Super Denise) run the same ZZ9500CX
# Denise adapter, which presents 4-bit RGB on R3..R0/G3..R0/B3..B0 and
# CSYNC-only sync: one capture configuration for both. Issue #76.
DENISE_ADAPTER = {
    "VCAP_RGB_MODE": "1",
    "VCAP_CSYNC_VSYNC": "1",
    "VCAP_FULLRATE_INT": "0",
    "CLKOUT0_DIVIDE_F": "16",
    "CLKOUT0_PHASE": "90",
    "CLKOUT1_PHASE": "270",
    "letterbox": True,
}

def check_writeback_provenance(rtl: str) -> None:
    """The writeback must not re-base in-flight rows on vc_saving_line.

    A 16-beat burst is 16 pixels; when AXI stalls push a row's writeback
    across the next capture line boundary, a live base redirects the
    remaining bursts to the new row's address with the old x offset -
    the periodic 16-pixel wrong-row box from issue #76's follow-up.
    """
    fragments = (
        "wire [23:0] vc_burst_base = (videocap_save_x == 0) ?",
        "m01_axi_awaddr_out  <= videocap_address + ((vc_burst_base + videocap_write_x)<<2);",
        "videocap_save_line_done <= vc_row_line;",
        "vc_row_base <= vc_saveaddr1;",
        "vc_row_line <= vc_saving_line;",
        "vc_row_bank <= vc_saving_bank;",
        ".buf_rbank(vc_row_bank),",
        "vc_saveaddr_line == vc_saving_line) begin",
        # A disable mid-row must not resume into the previous session's
        # frozen row: the re-enable path restarts at the row origin.
        "videocap_save_x <= 0;",
        # Filtered writeback writes a row only while its line is still in
        # the single line buffer; a scrolled-out row is skipped, not
        # written with another line's content.
        "wire vc_row_line_stale = !videocap_writeback_full_width &&",
        "videocap_save_x == 0 && vc_row_line_stale) begin",
        # Filtered capture banks its line buffer and hands rows over by
        # completed-line token: the writeback gets a full line of slack.
        "wire [11:0] vcap_line_payload_cap = (`VCAP_FULLRATE_INT != 0) ? {",
        "vcap_line_toggle, vcap_token_bank, vcap_token_y",
        "if (vcap_line_payload_axi[9:0] < videocap_ymax_sync)",
    )
    for fragment in fragments:
        if fragment not in rtl:
            raise SystemExit(
                "writeback provenance contract violated: missing "
                f"fragment: {fragment}"
            )


def main():
    rtl = RTL_PATH.read_text(encoding="utf-8")
    script = BUILD_SCRIPT.read_text(encoding="utf-8")

    # The committed default keeps ZORRO3 first in every ladder, so the
    # simultaneously-defined VARIANT_SUPERDENISE must not leak into the
    # video-slot capture configuration.
    committed = VariantConfig("committed default (zorro3)", rtl)
    check_variant(committed, dict(VIDEO_SLOT, bus_defines=("ZORRO3",)))

    expectations = {
        "zorro3": dict(VIDEO_SLOT, bus_defines=("ZORRO3", "VARIANT_Z3_FASTRAM")),
        "zorro3-nofast": dict(VIDEO_SLOT, bus_defines=("ZORRO3",)),
        "zorro2": dict(ZORRO2_SLOT, bus_defines=("ZORRO2",)),
        "zorro2-2mb": dict(
            ZORRO2_SLOT, bus_defines=("ZORRO2", "VARIANT_2MB")
        ),
        "a500": dict(DENISE_ADAPTER, bus_defines=("ZORRO2", "VARIANT_ZZ9500")),
        "a500-2mb": dict(
            DENISE_ADAPTER,
            bus_defines=("ZORRO2", "VARIANT_ZZ9500", "VARIANT_2MB"),
        ),
        "a500plus": dict(
            DENISE_ADAPTER,
            bus_defines=("ZORRO2", "VARIANT_SUPERDENISE"),
        ),
    }

    for variant, expect in expectations.items():
        spliced = splice_variant_block(
            rtl, variant_block_from_script(script, variant)
        )
        check_variant(VariantConfig(variant, spliced), expect)

    check_writeback_provenance(rtl)

    print("RTL capture contract checks passed")


if __name__ == "__main__":
    sys.exit(main())
