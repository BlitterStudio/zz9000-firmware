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
VIDEO_HEADER = (
    ROOT / "ZZ9000_proto.sdk" / "ZZ9000OS" / "src" / "video.h"
).read_text(encoding="utf-8")
MAIN = (
    ROOT / "ZZ9000_proto.sdk" / "ZZ9000OS" / "src" / "main.c"
).read_text(encoding="utf-8")


def require(source: str, fragment: str) -> None:
    if fragment not in source:
        raise SystemExit(f"missing aperture contract fragment: {fragment}")


for fragment in (
    "`define Z3_FASTRAM_ARM_BASE       32'h2fe00000",
    "`define BOOT_ROM_ADDRESS          32'h3fef0000",
    "`define TX_FRAME_ADDRESS          32'h3ff10000",
    "`define RX_FRAME_ADDRESS          32'h3ff20000",
    "`define RX_BACKLOG_ADDRESS        32'h3ff30000",
    "`define USB_BLOCK_STORAGE_ADDRESS 32'h3ff70000",
    "localparam VCAP_DIAG_VARIANT_FASTRAM_2FE_BASE = 1'b1;",
    "7'h00, VCAP_DIAG_VARIANT_FASTRAM_2FE_BASE",
    "MNTZORRO_CAPABILITY_VALUE",
    "(VCAP_DIAG_VARIANT_FASTRAM_2FE_BASE ? 32'h80000000 : 32'h00000000) |\n      32'h40000000;",
    "3'h7   : reg_data_out <= SDK_APERTURE_SIZE_VALUE |",
):
    require(RTL, fragment)

for fragment in (
    "SDK_REG_APERTURE_INFO = 16'h011c",
    "SDK_REG_APERTURE_INFO_LO = 16'h011e",
    "SDK_APERTURE_ACK_TOKEN = 16'ha502",
    "SDK_APERTURE_INFO_VALUE = 32'h5a020502",
    "SDK_APERTURE_SIZE_VALUE = 32'h00200000",
    "SDK_APERTURE_INFO_VALUE = 32'h5a020704",
    "SDK_APERTURE_SIZE_VALUE = 32'h00400000",
    "3'h6   : reg_data_out <= sdk_aperture_layout_ack ?",
    "32'ha5020001 : 32'h00000000",
    "3'h7   : reg_data_out <= SDK_APERTURE_SIZE_VALUE |",
):
    require(RTL, fragment)

for fragment in (
    "#define MNTZORRO_REG6 24",
    "#define MNTZORRO_REG7 28",
    "#define MNTZORRO_APERTURE_ACK_STATUS 0xa5020001UL",
    "#define MNTZORRO_CAP_Z3_FASTRAM_DDR_2FE00000 0x80000000UL",
    "#define MNTZORRO_CAP_GAMMA_LUT 0x40000000UL",
    "(MNTZORRO_CAP_Z3_FASTRAM_DDR_2FE00000 | MNTZORRO_CAP_GAMMA_LUT)",
):
    require(HEADER, fragment)

for fragment in (
    "wire video_control_axi_op33_event =",
    "axi_reg2[7:0] == 8'd33",
    "axi_reg3[5:0] == 6'b000000",
    "videocap_address_valid <= 1'b0;",
    "m01_axi_awaddr_out  <= videocap_address +",
):
    require(RTL, fragment)
require(VIDEO_HEADER, "#define MNTVF_OP_VIDEOCAP_ADDRESS 33")

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
