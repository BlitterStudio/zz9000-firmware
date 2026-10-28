#!/usr/bin/env python3
"""Copy RasterIX for XSim and apply declaration-order-only compatibility fixes."""
from pathlib import Path
import shutil
import sys


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if text.count(old) != 1:
        raise RuntimeError(f"unexpected source shape in {path}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: prepare_xsim_sources.py <repo-root> <output-root>")
    source = Path(sys.argv[1]) / "rtl" / "rasterix"
    output = Path(sys.argv[2])
    if output.exists():
        shutil.rmtree(output)
    shutil.copytree(source, output)

    replace_once(
        output / "Float/rtl/float/FloatAdd.v",
        "    wire [MANTISSA_ONE_POS_SIZE - 1 : 0] three_exponentCorrection;\n",
        "    reg  [MANTISSA_CALC_SIZE - 1 : 0] two_mantissaSum;\n"
        "    reg                               two_mantissaSumSign;\n"
        "    reg  [EXPONENT_SIZE - 1 : 0]      two_bigNumberExponent;\n"
        "    reg  [EXPONENT_SIZE - 1 : 0]      two_smallNumberExponent;\n"
        "    wire [MANTISSA_ONE_POS_SIZE - 1 : 0] three_exponentCorrection;\n",
    )
    replace_once(
        output / "Float/rtl/float/FloatAdd.v",
        "    reg  [MANTISSA_CALC_SIZE - 1 : 0] two_mantissaSum;\n"
        "    reg                               two_mantissaSumSign;\n"
        "    reg  [EXPONENT_SIZE - 1 : 0]      two_bigNumberExponent;\n"
        "    reg  [EXPONENT_SIZE - 1 : 0]      two_smallNumberExponent;\n"
        "    always @(posedge clk)\n"
        "    if (ce) begin : Calc\n",
        "    always @(posedge clk)\n"
        "    if (ce) begin : Calc\n",
    )
    replace_once(
        output / "Float/rtl/float/IntToFloat.v",
        "    wire [UNSIGNED_WORK_INT_SIZE_LOG2 - 1 : 0] two_exponent;\n",
        "    reg  [UNSIGNED_WORK_INT_SIZE - 1 : 0] one_number;\n"
        "    reg                                 one_sign;\n"
        "    wire [UNSIGNED_WORK_INT_SIZE_LOG2 - 1 : 0] two_exponent;\n",
    )
    replace_once(
        output / "Float/rtl/float/IntToFloat.v",
        "    reg  [UNSIGNED_WORK_INT_SIZE - 1 : 0]    one_number;\n"
        "    reg                                 one_sign;\n"
        "    always @(posedge clk)\n"
        "    if (ce) begin : Prepare\n",
        "    always @(posedge clk)\n"
        "    if (ce) begin : Prepare\n",
    )
    replace_once(
        output / "RasterIX/TextureCacheDirectMappedContext.v",
        "    function [ADDR_WIDTH - 1 : 0] GetCacheGroupAddress;\n",
        "    reg  [DATA_WIDTH - 1 : 0]       rCacheMemory [0 : (CACHE_SIZE / (DATA_WIDTH / 8)) - 1];\n\n"
        "    function [ADDR_WIDTH - 1 : 0] GetCacheGroupAddress;\n",
    )
    replace_once(
        output / "RasterIX/TextureCacheDirectMappedContext.v",
        "    reg  [DATA_WIDTH - 1 : 0]       rCacheMemory [0 : (CACHE_SIZE / (DATA_WIDTH / 8)) - 1];\n\n"
        "    reg                             rSkidValid;\n",
        "    reg                             rSkidValid;\n",
    )
    render = output / "RasterIX/RasterIXRenderCore.v"
    replace_once(
        render,
        "    localparam RASTERIZER_CONCAT_WIDTH = (4 * SCREEN_POS_WIDTH) + INDEX_WIDTH + 1 + RR_CMD_SIZE;\n",
        "    localparam RASTERIZER_CONCAT_WIDTH = (4 * SCREEN_POS_WIDTH) + INDEX_WIDTH + 1 + RR_CMD_SIZE;\n"
        "    wire                                    alrp_tready;\n"
        "    wire                                    alrp_tvalid;\n"
        "    wire                                    alrp_tpixel;\n"
        "    wire                                    fragment_stream_out_tvalid;\n"
        "    wire                                    fragment_stream_out_tready;\n"
        "    wire                                    framebuffer_pfp_wvalid;\n"
        "    wire                                    framebuffer_pfp_wready;\n",
    )
    replace_once(
        render,
        "    wire                                    alrp_tready;\n"
        "    wire                                    alrp_tvalid;\n"
        "    wire                                    alrp_tpixel;\n"
        "    wire                                    alrp_tlast;\n",
        "    wire                                    alrp_tlast;\n",
    )
    replace_once(
        render,
        "    wire fragment_stream_out_tvalid;\n"
        "    wire fragment_stream_out_tready;\n"
        "    wire [(FRAGMENT_STREAM_WIDTH + PIXEL_WIDTH + DEPTH_WIDTH + STENCIL_WIDTH) - 1 : 0] fragment_stream_out_tdata;\n",
        "    wire [(FRAGMENT_STREAM_WIDTH + PIXEL_WIDTH + DEPTH_WIDTH + STENCIL_WIDTH) - 1 : 0] fragment_stream_out_tdata;\n",
    )
    replace_once(
        render,
        "    wire                            framebuffer_pfp_wvalid;\n"
        "    wire                            framebuffer_pfp_wlast;\n"
        "    wire                            framebuffer_pfp_wready;\n",
        "    wire                            framebuffer_pfp_wlast;\n",
    )
    reader = output / "RasterIX/TextureReader.v"
    replace_once(
        reader,
        "    wire                         fifoReady;\n"
        "    StreamConcatFifo #(\n",
        "    wire                         fifoReady;\n"
        "    wire [TEXEL_WIDTH + 2 : 0]   fifoData;\n"
        "    assign { fifoTexelPos, fifoCmd, fifoTexel } = fifoData;\n"
        "    StreamConcatFifo #(\n",
    )
    replace_once(
        reader,
        "        .m_stream_tdata({\n"
        "            1'b0,\n"
        "            1'b0,\n"
        "            fifoTexelPos,\n"
        "            fifoCmd,\n"
        "            fifoTexel\n"
        "        }),\n",
        "        .m_stream_tdata(fifoData),\n",
    )
    core_if = output / "RasterIX/RasterIXCoreIF.v"
    replace_once(
        core_if,
        "        if (ENABLE_DEPTH_BUFFER)\n"
        "        begin\n"
        "            wire [PIXEL_PER_BEAT - 1 : 0] m_depthbuffer_unconverted_axis_tstrb;\n",
        "        if (ENABLE_DEPTH_BUFFER)\n"
        "        begin : genDepthBuffer\n"
        "            wire [PIXEL_PER_BEAT - 1 : 0] m_depthbuffer_unconverted_axis_tstrb;\n",
    )
    print(f"prepared XSim-only RasterIX copy at {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
