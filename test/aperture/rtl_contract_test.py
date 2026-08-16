#!/usr/bin/env python3
"""Keep the RTL bootstrap registers aligned with the C layout contract."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RTL = (ROOT / "mntzorro.v").read_text(encoding="utf-8")
HEADER = (
    ROOT / "ZZ9000_proto.sdk" / "ZZ9000OS" / "src" / "mntzorro.h"
).read_text(encoding="utf-8")
REGS_HEADER = (
    ROOT / "ZZ9000_proto.sdk" / "ZZ9000OS" / "src" / "zz_regs.h"
).read_text(encoding="utf-8")
MAIN = (
    ROOT / "ZZ9000_proto.sdk" / "ZZ9000OS" / "src" / "main.c"
).read_text(encoding="utf-8")


def require(source: str, fragment: str) -> None:
    if fragment not in source:
        raise SystemExit(f"missing aperture contract fragment: {fragment}")


for fragment in (
    "SDK_REG_APERTURE_INFO = 16'h011c",
    "SDK_REG_APERTURE_INFO_LO = 16'h011e",
    "SDK_APERTURE_ACK_TOKEN = 16'ha501",
    "SDK_APERTURE_INFO_VALUE = 32'h5a010502",
    "SDK_APERTURE_SIZE_VALUE = 32'h00200000",
    "SDK_APERTURE_INFO_VALUE = 32'h5a010704",
    "SDK_APERTURE_SIZE_VALUE = 32'h00400000",
    "3'h6   : reg_data_out <= sdk_aperture_layout_ack ?",
    "3'h7   : reg_data_out <= SDK_APERTURE_SIZE_VALUE",
):
    require(RTL, fragment)

for fragment in (
    "#define MNTZORRO_REG6 24",
    "#define MNTZORRO_REG7 28",
    "#define MNTZORRO_APERTURE_ACK_STATUS 0xa5010001UL",
):
    require(HEADER, fragment)

require(REGS_HEADER, "#define ZZ_FW_CAP_Z2_APERTURE_LAYOUT (1U << 2)")

require(MAIN, "uint8_t aperture_ack_poll_divider = 0")
require(MAIN, "aperture_ack_poll_divider++ == 0U")
require(MAIN, "static void activate_aperture_layout_if_acknowledged(void)")
for command, handler in (
    ("case REG_ZZ_ACC_OP:", "handle_acc_op(zdata);"),
    ("case REG_ZZ_BLITTER_DMA_OP:", "handle_blitter_dma_op(video_state, zdata);"),
):
    command_start = MAIN.index(command)
    handler_start = MAIN.index(handler, command_start)
    command_block = MAIN[command_start:handler_start]
    require(command_block, "activate_aperture_layout_if_acknowledged();")
ack_start = MAIN.index("static void activate_aperture_layout_if_acknowledged(void)")
ack_end = MAIN.index("void handle_amiga_reset", ack_start)
if "clear_runtime_gfxdata();" in MAIN[ack_start:ack_end]:
    raise SystemExit("aperture ACK transition clears host-owned GFXData")

print("RTL aperture contract checks passed")
