// SPDX-License-Identifier: GPL-3.0-or-later
// ZZ9000 RasterIX EF control/status wrapper. Commands enter RasterIX through
// the separate AXI-Stream FIFO MMIO peripheral; this module has no DDR fetcher.
module zz9k_rasterix #(
    parameter integer C_S_AXI_ADDR_WIDTH = 16,
    parameter integer C_S_AXI_DATA_WIDTH = 32
) (
    input wire s_axi_aclk,
    input wire s_axi_aresetn,
    input wire [C_S_AXI_ADDR_WIDTH-1:0] s_axi_awaddr,
    input wire [2:0] s_axi_awprot,
    input wire s_axi_awvalid,
    output wire s_axi_awready,
    input wire [C_S_AXI_DATA_WIDTH-1:0] s_axi_wdata,
    input wire [C_S_AXI_DATA_WIDTH/8-1:0] s_axi_wstrb,
    input wire s_axi_wvalid,
    output wire s_axi_wready,
    output wire [1:0] s_axi_bresp,
    output wire s_axi_bvalid,
    input wire s_axi_bready,
    input wire [C_S_AXI_ADDR_WIDTH-1:0] s_axi_araddr,
    input wire [2:0] s_axi_arprot,
    input wire s_axi_arvalid,
    output wire s_axi_arready,
    output wire [C_S_AXI_DATA_WIDTH-1:0] s_axi_rdata,
    output wire [1:0] s_axi_rresp,
    output wire s_axi_rvalid,
    input wire s_axi_rready,

    input wire s_cmd_axis_tvalid,
    output wire s_cmd_axis_tready,
    input wire s_cmd_axis_tlast,
    input wire [31:0] s_cmd_axis_tdata,

    input wire m_axi_aclk,
    input wire m_axi_aresetn,
    output wire [5:0] m_axi_awid,
    output wire [31:0] m_axi_awaddr,
    output wire [7:0] m_axi_awlen,
    output wire [2:0] m_axi_awsize,
    output wire [1:0] m_axi_awburst,
    output wire m_axi_awlock,
    output wire [3:0] m_axi_awcache,
    output wire [2:0] m_axi_awprot,
    output wire m_axi_awvalid,
    input wire m_axi_awready,
    output wire [63:0] m_axi_wdata,
    output wire [7:0] m_axi_wstrb,
    output wire m_axi_wlast,
    output wire m_axi_wvalid,
    input wire m_axi_wready,
    input wire [5:0] m_axi_bid,
    input wire [1:0] m_axi_bresp,
    input wire m_axi_bvalid,
    output wire m_axi_bready,
    output wire [5:0] m_axi_arid,
    output wire [31:0] m_axi_araddr,
    output wire [7:0] m_axi_arlen,
    output wire [2:0] m_axi_arsize,
    output wire [1:0] m_axi_arburst,
    output wire m_axi_arlock,
    output wire [3:0] m_axi_arcache,
    output wire [2:0] m_axi_arprot,
    output wire m_axi_arvalid,
    input wire m_axi_arready,
    input wire [5:0] m_axi_rid,
    input wire [63:0] m_axi_rdata,
    input wire [1:0] m_axi_rresp,
    input wire m_axi_rlast,
    input wire m_axi_rvalid,
    output wire m_axi_rready
);
    localparam [31:0] IDENTITY = 32'h5249_5831; // RIX1
    localparam [31:0] VERSION = 32'h0002_0000;
    reg [C_S_AXI_ADDR_WIDTH-1:0] write_addr;
    reg [31:0] write_data;
    reg [3:0] write_strb;
    reg write_addr_valid;
    reg write_data_valid;
    reg write_response_valid;
    reg [31:0] read_data;
    reg read_response_valid;
    reg enable_requested;
    reg soft_reset;
    reg [15:0] writes_outstanding;
    reg [15:0] reads_outstanding;
    reg [31:0] dbg_aw_count;
    reg [31:0] dbg_w_count;
    reg [31:0] dbg_b_count;
    reg [31:0] dbg_ar_count;
    reg [31:0] dbg_r_count;
    reg [31:0] dbg_last_araddr;
    reg [5:0] dbg_last_arid;

    wire control_write = write_addr_valid && write_data_valid && !write_response_valid &&
                         write_strb[0] && (write_addr[5:2] == 4'h2);
    wire renderer_resetn = m_axi_aresetn && !soft_reset;
    wire renderer_busy;
    wire renderer_triangle_rendering;
    wire renderer_stall;
    wire rix_response_valid;
    wire rix_response_last;
    wire [31:0] rix_response_data;
    wire [31:0] rix_fb_addr;
    wire [19:0] rix_fb_size;
    wire rix_swap_fb;
    wire rix_swap_fb_enable_vsync;
    wire write_issue = m_axi_awvalid && m_axi_awready;
    wire write_complete = m_axi_bvalid && m_axi_bready;
    wire read_issue = m_axi_arvalid && m_axi_arready;
    wire read_complete = m_axi_rvalid && m_axi_rready && m_axi_rlast;
    wire idle = !renderer_busy && (writes_outstanding == 0) && (reads_outstanding == 0);

    assign s_axi_awready = !write_addr_valid && !write_response_valid;
    assign s_axi_wready = !write_data_valid && !write_response_valid;
    assign s_axi_bvalid = write_response_valid;
    assign s_axi_bresp = 2'b00;
    assign s_axi_arready = !read_response_valid;
    assign s_axi_rvalid = read_response_valid;
    assign s_axi_rdata = read_data;
    assign s_axi_rresp = 2'b00;

    always @(*) begin
        case (s_axi_araddr[7:2])
            6'h00: read_data = IDENTITY;
            6'h01: read_data = VERSION;
            6'h02: read_data = {29'd0, soft_reset, 1'b0, enable_requested};
            6'h03: read_data = {8'd0, reads_outstanding[7:0], writes_outstanding[7:0],
                                 3'd0, renderer_stall, renderer_triangle_rendering,
                                 renderer_busy, idle, enable_requested};
            6'h04: read_data = {28'd0, rix_response_last, rix_response_valid,
                                 s_cmd_axis_tready, s_cmd_axis_tvalid};
            6'h05: read_data = rix_response_data;
            6'h0e: read_data = dbg_aw_count;
            6'h0f: read_data = dbg_w_count;
            6'h10: read_data = dbg_b_count;
            6'h11: read_data = dbg_ar_count;
            6'h12: read_data = dbg_r_count;
            6'h13: read_data = dbg_last_araddr;
            6'h14: read_data = {26'd0, dbg_last_arid};
            default: read_data = 32'd0;
        endcase
    end

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            write_addr <= 0;
            write_data <= 0;
            write_strb <= 0;
            write_addr_valid <= 0;
            write_data_valid <= 0;
            write_response_valid <= 0;
            read_response_valid <= 0;
            enable_requested <= 0;
            soft_reset <= 0;
            writes_outstanding <= 0;
            reads_outstanding <= 0;
            dbg_aw_count <= 0;
            dbg_w_count <= 0;
            dbg_b_count <= 0;
            dbg_ar_count <= 0;
            dbg_r_count <= 0;
            dbg_last_araddr <= 0;
            dbg_last_arid <= 0;
        end else begin
            soft_reset <= 0;
            if (s_axi_awvalid && s_axi_awready) begin
                write_addr <= s_axi_awaddr;
                write_addr_valid <= 1;
            end
            if (s_axi_wvalid && s_axi_wready) begin
                write_data <= s_axi_wdata;
                write_strb <= s_axi_wstrb;
                write_data_valid <= 1;
            end
            if (write_addr_valid && write_data_valid && !write_response_valid) begin
                write_addr_valid <= 0;
                write_data_valid <= 0;
                write_response_valid <= 1;
                if (control_write) begin
                    enable_requested <= write_data[0];
                    soft_reset <= write_data[1];
                end
            end
            if (write_response_valid && s_axi_bready)
                write_response_valid <= 0;
            if (s_axi_arvalid && s_axi_arready)
                read_response_valid <= 1;
            if (read_response_valid && s_axi_rready)
                read_response_valid <= 0;

            // An address and a completing response in the same cycle cancel.
            if (write_issue && !write_complete)
                writes_outstanding <= writes_outstanding + 1'b1;
            else if (write_complete && !write_issue && (writes_outstanding != 0))
                writes_outstanding <= writes_outstanding - 1'b1;
            if (read_issue && !read_complete)
                reads_outstanding <= reads_outstanding + 1'b1;
            else if (read_complete && !read_issue && (reads_outstanding != 0))
                reads_outstanding <= reads_outstanding - 1'b1;
            if (write_issue)
                dbg_aw_count <= dbg_aw_count + 1'b1;
            if (m_axi_wvalid && m_axi_wready)
                dbg_w_count <= dbg_w_count + 1'b1;
            if (write_complete)
                dbg_b_count <= dbg_b_count + 1'b1;
            if (read_issue) begin
                dbg_ar_count <= dbg_ar_count + 1'b1;
                dbg_last_araddr <= m_axi_araddr;
                dbg_last_arid <= m_axi_arid;
            end
            if (m_axi_rvalid && m_axi_rready)
                dbg_r_count <= dbg_r_count + 1'b1;
            if (soft_reset) begin
                writes_outstanding <= 0;
                reads_outstanding <= 0;
                dbg_aw_count <= 0;
                dbg_w_count <= 0;
                dbg_b_count <= 0;
                dbg_ar_count <= 0;
                dbg_r_count <= 0;
            end
        end
    end

    // The AXI FIFO MM-S presents full packets only after TLF; backpressure is
    // intentional while disabled/reset so a producer cannot start the engine.
    assign s_cmd_axis_tready = enable_requested && !soft_reset && renderer_ready;
    wire renderer_ready;
    RasterIX_EF #(
        .SUB_PIXEL_CALC_PRECISION(8),
        .ENABLE_STENCIL_BUFFER(0),
        .ENABLE_DEPTH_BUFFER(1),
        .TMU_COUNT(1),
        .ENABLE_MIPMAPPING(1),
        .ENABLE_TEXTURE_FILTERING(1),
        .TEXTURE_PAGE_SIZE(2048),
        .ENABLE_EXTERNAL_TEXTURE_MEMORY(1),
        .CACHE_SIZE(8192),
        .ENABLE_FOG(1),
        .MAX_TEXTURE_SIZE(256),
        .ADDR_WIDTH(32),
        .ID_WIDTH(6),
        .DATA_WIDTH(64),
        .STRB_WIDTH(8),
        // Merge the framebuffer masters' single-beat transactions into
        // INCR bursts (see rtl/rasterix/PATCHES.md).
        .ENABLE_MEMORY_COALESCING(1),
        .RASTERIZER_FLOAT_PRECISION(24),
        .RASTERIZER_FIXPOINT_PRECISION(18),
        .RASTERIZER_ENABLE_FLOAT_INTERPOLATION(0)
    ) renderer (
        .aclk(m_axi_aclk), .resetn(renderer_resetn),
        .s_cmd_axis_tvalid(s_cmd_axis_tvalid && enable_requested && !soft_reset),
        .s_cmd_axis_tready(renderer_ready), .s_cmd_axis_tlast(s_cmd_axis_tlast),
        .s_cmd_axis_tdata(s_cmd_axis_tdata),
        .m_cmd_resp_axis_tvalid(rix_response_valid), .m_cmd_resp_axis_tready(1'b1),
        .m_cmd_resp_axis_tlast(rix_response_last), .m_cmd_resp_axis_tdata(rix_response_data),
        .swap_fb(rix_swap_fb), .swap_fb_enable_vsync(rix_swap_fb_enable_vsync),
        .fb_addr(rix_fb_addr), .fb_size(rix_fb_size), .fb_swapped(1'b1),
        .m_axi_awid(m_axi_awid), .m_axi_awaddr(m_axi_awaddr), .m_axi_awlen(m_axi_awlen),
        .m_axi_awsize(m_axi_awsize), .m_axi_awburst(m_axi_awburst), .m_axi_awlock(m_axi_awlock),
        .m_axi_awcache(m_axi_awcache), .m_axi_awprot(m_axi_awprot), .m_axi_awvalid(m_axi_awvalid),
        .m_axi_awready(m_axi_awready), .m_axi_wdata(m_axi_wdata), .m_axi_wstrb(m_axi_wstrb),
        .m_axi_wlast(m_axi_wlast), .m_axi_wvalid(m_axi_wvalid), .m_axi_wready(m_axi_wready),
        .m_axi_bid(m_axi_bid), .m_axi_bresp(m_axi_bresp), .m_axi_bvalid(m_axi_bvalid), .m_axi_bready(m_axi_bready),
        .m_axi_arid(m_axi_arid), .m_axi_araddr(m_axi_araddr), .m_axi_arlen(m_axi_arlen),
        .m_axi_arsize(m_axi_arsize), .m_axi_arburst(m_axi_arburst), .m_axi_arlock(m_axi_arlock),
        .m_axi_arcache(m_axi_arcache), .m_axi_arprot(m_axi_arprot), .m_axi_arvalid(m_axi_arvalid),
        .m_axi_arready(m_axi_arready), .m_axi_rid(m_axi_rid), .m_axi_rdata(m_axi_rdata),
        .m_axi_rresp(m_axi_rresp), .m_axi_rlast(m_axi_rlast), .m_axi_rvalid(m_axi_rvalid),
        .m_axi_rready(m_axi_rready), .perfBusy(renderer_busy),
        .perfTriangleRendering(renderer_triangle_rendering), .perfRasterizerStall(renderer_stall)
    );
endmodule
