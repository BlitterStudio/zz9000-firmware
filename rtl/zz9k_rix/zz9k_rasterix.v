// SPDX-License-Identifier: GPL-3.0-or-later
// ZZ9000-specific integration for the vendored RasterIX renderer.
//
// RasterIX uses m_axi for texture/framebuffer traffic.  Command buffers use
// the independent 64-bit m_cmd_axi read master; axi_interconnect_2 arbitrates
// both below its capture S00 slot, so capture writeback retains priority.
`ifndef RASTERIX_TMU_COUNT
`define RASTERIX_TMU_COUNT 1
`endif

`ifndef RASTERIX_FRAMEBUFFER_SIZE_IN_PIXEL_LG
`define RASTERIX_FRAMEBUFFER_SIZE_IN_PIXEL_LG 14
`endif

module zz9k_rasterix #(
    parameter integer C_S_AXI_ADDR_WIDTH = 16,
    parameter integer C_S_AXI_DATA_WIDTH = 32
) (
    input  wire                               s_axi_aclk,
    input  wire                               s_axi_aresetn,
    input  wire [C_S_AXI_ADDR_WIDTH-1:0]      s_axi_awaddr,
    input  wire [2:0]                         s_axi_awprot,
    input  wire                               s_axi_awvalid,
    output wire                               s_axi_awready,
    input  wire [C_S_AXI_DATA_WIDTH-1:0]      s_axi_wdata,
    input  wire [C_S_AXI_DATA_WIDTH/8-1:0]    s_axi_wstrb,
    input  wire                               s_axi_wvalid,
    output wire                               s_axi_wready,
    output wire [1:0]                         s_axi_bresp,
    output wire                               s_axi_bvalid,
    input  wire                               s_axi_bready,
    input  wire [C_S_AXI_ADDR_WIDTH-1:0]      s_axi_araddr,
    input  wire [2:0]                         s_axi_arprot,
    input  wire                               s_axi_arvalid,
    output wire                               s_axi_arready,
    output wire [C_S_AXI_DATA_WIDTH-1:0]      s_axi_rdata,
    output wire [1:0]                         s_axi_rresp,
    output wire                               s_axi_rvalid,
    input  wire                               s_axi_rready,

    input  wire                               m_axi_aclk,
    input  wire                               m_axi_aresetn,
    output wire [3:0]                         m_axi_awid,
    output wire [31:0]                        m_axi_awaddr,
    output wire [7:0]                         m_axi_awlen,
    output wire [2:0]                         m_axi_awsize,
    output wire [1:0]                         m_axi_awburst,
    output wire                               m_axi_awlock,
    output wire [3:0]                         m_axi_awcache,
    output wire [2:0]                         m_axi_awprot,
    output wire                               m_axi_awvalid,
    input  wire                               m_axi_awready,
    output wire [31:0]                        m_axi_wdata,
    output wire [3:0]                         m_axi_wstrb,
    output wire                               m_axi_wlast,
    output wire                               m_axi_wvalid,
    input  wire                               m_axi_wready,
    input  wire [3:0]                         m_axi_bid,
    input  wire [1:0]                         m_axi_bresp,
    input  wire                               m_axi_bvalid,
    output wire                               m_axi_bready,
    output wire [3:0]                         m_axi_arid,
    output wire [31:0]                        m_axi_araddr,
    output wire [7:0]                         m_axi_arlen,
    output wire [2:0]                         m_axi_arsize,
    output wire [1:0]                         m_axi_arburst,
    output wire                               m_axi_arlock,
    output wire [3:0]                         m_axi_arcache,
    output wire [2:0]                         m_axi_arprot,
    output wire                               m_axi_arvalid,
    input  wire                               m_axi_arready,
    input  wire [3:0]                         m_axi_rid,
    input  wire [31:0]                        m_axi_rdata,
    input  wire [1:0]                         m_axi_rresp,
    input  wire                               m_axi_rlast,
    input  wire                               m_axi_rvalid,
    output wire                               m_axi_rready,

    output wire [3:0]                         m_cmd_axi_arid,
    output wire [31:0]                        m_cmd_axi_araddr,
    output wire [7:0]                         m_cmd_axi_arlen,
    output wire [2:0]                         m_cmd_axi_arsize,
    output wire [1:0]                         m_cmd_axi_arburst,
    output wire                               m_cmd_axi_arlock,
    output wire [3:0]                         m_cmd_axi_arcache,
    output wire [2:0]                         m_cmd_axi_arprot,
    output wire                               m_cmd_axi_arvalid,
    input  wire                               m_cmd_axi_arready,
    input  wire [3:0]                         m_cmd_axi_rid,
    input  wire [63:0]                        m_cmd_axi_rdata,
    input  wire [1:0]                         m_cmd_axi_rresp,
    input  wire                               m_cmd_axi_rlast,
    input  wire                               m_cmd_axi_rvalid,
    output wire                               m_cmd_axi_rready
);
    localparam [31:0] IDENTITY = 32'h5249_5831; // "RIX1"
    localparam [31:0] VERSION = 32'h0001_0001;

    reg [C_S_AXI_ADDR_WIDTH-1:0] write_addr;
    reg [31:0] write_data;
    reg [3:0] write_strb;
    reg write_addr_valid;
    reg write_data_valid;
    reg write_response_valid;
    reg [31:0] read_data;
    reg read_response_valid;
    reg enable_requested;
    reg renderer_clock_enable;
    reg soft_reset_pending;
    reg soft_reset_active;
    reg [31:0] command_word;
    reg [31:0] command_fifo [0:15];
    reg command_last_fifo [0:15];
    reg [3:0] command_write_ptr;
    reg [3:0] command_read_ptr;
    reg [4:0] command_count;
    reg [7:0] write_transactions;
    reg [7:0] read_transactions;
    reg [31:0] cmd_addr;
    reg [31:0] cmd_words;
    reg [31:0] submissions_accepted;
    reg [31:0] submissions_completed;
    reg completion_pending;
    reg error_sticky;
    reg irq_status;
    reg [31:0] error_address;
    reg pio_rejected;
    reg start_rejected;

    wire renderer_clock;
    wire renderer_resetn;
    wire command_fifo_empty;
    wire command_fifo_full;
    wire command_pop;
    wire command_push;
    wire pio_push_request;
    wire engine_idle;
    wire renderer_busy;
    wire renderer_triangle_rendering;
    wire renderer_stall;
    wire rix_command_valid;
    wire rix_command_ready;
    wire rix_command_last;
    wire [31:0] rix_command_data;
    wire rix_response_valid;
    wire rix_response_last;
    wire [31:0] rix_response_data;

    wire fetch_submit_ready;
    wire fetch_submit_request;
    wire fetch_submit_fire;
    wire fetch_busy;
    wire fetch_command_valid;
    wire fetch_command_ready;
    wire fetch_command_last;
    wire [31:0] fetch_command_data;
    wire fetch_submission_done;
    wire fetch_read_error;
    wire [31:0] fetch_read_error_addr;
    wire completion_ack;

    wire [3:0] rix_m_axi_awid;
    wire [31:0] rix_m_axi_awaddr;
    wire [7:0] rix_m_axi_awlen;
    wire [2:0] rix_m_axi_awsize;
    wire [1:0] rix_m_axi_awburst;
    wire rix_m_axi_awlock;
    wire [3:0] rix_m_axi_awcache;
    wire [2:0] rix_m_axi_awprot;
    wire rix_m_axi_awvalid;
    wire [31:0] rix_m_axi_wdata;
    wire [3:0] rix_m_axi_wstrb;
    wire rix_m_axi_wlast;
    wire rix_m_axi_wvalid;
    wire rix_m_axi_bready;
    wire [3:0] rix_m_axi_arid;
    wire [31:0] rix_m_axi_araddr;
    wire [7:0] rix_m_axi_arlen;
    wire [2:0] rix_m_axi_arsize;
    wire [1:0] rix_m_axi_arburst;
    wire rix_m_axi_arlock;
    wire [3:0] rix_m_axi_arcache;
    wire [2:0] rix_m_axi_arprot;
    wire rix_m_axi_arvalid;
    wire rix_m_axi_rready;
    wire rix_swap_fb;
    wire rix_swap_fb_enable_vsync;
    wire [31:0] rix_fb_addr;
    wire [19:0] rix_fb_size;

    assign command_fifo_empty = (command_count == 0);
    assign command_fifo_full = (command_count == 16);
    assign engine_idle = !renderer_busy && command_fifo_empty && !fetch_busy &&
                         !completion_pending && (write_transactions == 0) &&
                         (read_transactions == 0);
    assign renderer_resetn = m_axi_aresetn && !soft_reset_active;

    // Keep reset visible through the gated clock.  Otherwise an initially
    // disabled BUFGCE prevents renderer state from observing reset before the
    // first submission.
    BUFGCE renderer_clock_gate (
        .I(m_axi_aclk),
        .CE(renderer_clock_enable),
        .O(renderer_clock)
    );

    assign s_axi_awready = !write_addr_valid && !write_response_valid;
    assign s_axi_wready = !write_data_valid && !write_response_valid;
    assign s_axi_bvalid = write_response_valid;
    assign s_axi_bresp = 2'b00;
    assign s_axi_arready = !read_response_valid;
    assign s_axi_rvalid = read_response_valid;
    assign s_axi_rdata = read_data;
    assign s_axi_rresp = 2'b00;

    assign pio_push_request = write_addr_valid && write_data_valid &&
                              !write_response_valid && write_strb[0] &&
                              (write_addr[5:2] == 4'h5) && write_data[0];
    assign command_push = pio_push_request && !fetch_busy && !command_fifo_full;
    assign fetch_submit_request = write_addr_valid && write_data_valid &&
                                  !write_response_valid && write_strb[0] &&
                                  (write_addr[5:2] == 4'h9) && write_data[0];
    assign fetch_submit_fire = fetch_submit_request && fetch_submit_ready &&
                               (cmd_addr[2:0] == 3'd0) && (cmd_words != 0);

    assign rix_command_valid = renderer_clock_enable &&
                               (fetch_command_valid || (!fetch_busy && !command_fifo_empty));
    assign rix_command_data = fetch_command_valid ? fetch_command_data :
                              command_fifo[command_read_ptr];
    assign rix_command_last = fetch_command_valid ? fetch_command_last :
                              command_last_fifo[command_read_ptr];
    assign fetch_command_ready = renderer_clock_enable && fetch_command_valid &&
                                 rix_command_ready;
    assign command_pop = renderer_clock_enable && !fetch_busy &&
                         !command_fifo_empty && rix_command_ready;

    // RasterIX replies are currently not part of the ARM ABI.  Keeping their
    // ready channel asserted prevents pruning and is safe: replies have no
    // target-memory side effects.
    wire rix_response_ready = 1'b1;

    assign m_axi_awid = rix_m_axi_awid;
    assign m_axi_awaddr = rix_m_axi_awaddr;
    assign m_axi_awlen = rix_m_axi_awlen;
    assign m_axi_awsize = rix_m_axi_awsize;
    assign m_axi_awburst = rix_m_axi_awburst;
    assign m_axi_awlock = rix_m_axi_awlock;
    assign m_axi_awcache = rix_m_axi_awcache;
    assign m_axi_awprot = rix_m_axi_awprot;
    assign m_axi_awvalid = renderer_clock_enable && rix_m_axi_awvalid;
    assign m_axi_wdata = rix_m_axi_wdata;
    assign m_axi_wstrb = rix_m_axi_wstrb;
    assign m_axi_wlast = rix_m_axi_wlast;
    assign m_axi_wvalid = renderer_clock_enable && rix_m_axi_wvalid;
    assign m_axi_bready = renderer_clock_enable && rix_m_axi_bready;
    assign m_axi_arid = rix_m_axi_arid;
    assign m_axi_araddr = rix_m_axi_araddr;
    assign m_axi_arlen = rix_m_axi_arlen;
    assign m_axi_arsize = rix_m_axi_arsize;
    assign m_axi_arburst = rix_m_axi_arburst;
    assign m_axi_arlock = rix_m_axi_arlock;
    assign m_axi_arcache = rix_m_axi_arcache;
    assign m_axi_arprot = rix_m_axi_arprot;
    assign m_axi_arvalid = renderer_clock_enable && rix_m_axi_arvalid;
    assign m_axi_rready = renderer_clock_enable && rix_m_axi_rready;

    assign completion_ack = completion_pending && !renderer_busy &&
                            (write_transactions == 0);

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            write_addr_valid <= 1'b0;
            write_data_valid <= 1'b0;
            write_response_valid <= 1'b0;
            read_response_valid <= 1'b0;
            read_data <= 32'd0;
            enable_requested <= 1'b0;
            renderer_clock_enable <= 1'b1;
            soft_reset_pending <= 1'b0;
            soft_reset_active <= 1'b0;
            command_word <= 32'd0;
            command_write_ptr <= 4'd0;
            command_read_ptr <= 4'd0;
            command_count <= 5'd0;
            write_transactions <= 8'd0;
            read_transactions <= 8'd0;
            cmd_addr <= 32'd0;
            cmd_words <= 32'd0;
            submissions_accepted <= 32'd0;
            submissions_completed <= 32'd0;
            completion_pending <= 1'b0;
            error_sticky <= 1'b0;
            irq_status <= 1'b0;
            error_address <= 32'd0;
            pio_rejected <= 1'b0;
            start_rejected <= 1'b0;
        end else begin
            if (s_axi_awvalid && s_axi_awready) begin
                write_addr <= s_axi_awaddr;
                write_addr_valid <= 1'b1;
            end
            if (s_axi_wvalid && s_axi_wready) begin
                write_data <= s_axi_wdata;
                write_strb <= s_axi_wstrb;
                write_data_valid <= 1'b1;
            end
            if (write_response_valid && s_axi_bready)
                write_response_valid <= 1'b0;

            if (s_axi_arvalid && s_axi_arready) begin
                case (s_axi_araddr[5:2])
                    4'h0: read_data <= IDENTITY;
                    4'h1: read_data <= VERSION;
                    4'h2: read_data <= {29'd0, error_sticky, soft_reset_pending, enable_requested};
                    4'h3: read_data <= {read_transactions, write_transactions, 4'd0,
                                         irq_status, start_rejected, pio_rejected, error_sticky,
                                         completion_pending, fetch_submit_ready, fetch_busy,
                                         renderer_stall, renderer_triangle_rendering,
                                         renderer_busy, engine_idle, renderer_clock_enable};
                    4'h4: read_data <= command_word;
                    4'h5: read_data <= {27'd0, command_count};
                    4'h6: read_data <= {16'd0, rix_response_data[15:0]};
                    4'h7: read_data <= cmd_addr;
                    4'h8: read_data <= cmd_words;
                    4'h9: read_data <= {31'd0, fetch_submit_ready};
                    4'hA: read_data <= submissions_accepted;
                    4'hB: read_data <= submissions_completed;
                    4'hC: read_data <= {29'd0, start_rejected, pio_rejected, error_sticky};
                    4'hD: read_data <= error_address;
                    default: read_data <= 32'd0;
                endcase
                read_response_valid <= 1'b1;
            end else if (read_response_valid && s_axi_rready) begin
                read_response_valid <= 1'b0;
            end

            if (write_addr_valid && write_data_valid && !write_response_valid) begin
                if (write_strb[0]) begin
                    case (write_addr[5:2])
                        4'h2: begin
                            enable_requested <= write_data[0];
                            if (write_data[0])
                                renderer_clock_enable <= 1'b1;
                            if (write_data[1] && engine_idle) begin
                                soft_reset_pending <= 1'b1;
                                renderer_clock_enable <= 1'b1;
                            end
                            if (write_data[2]) begin
                                error_sticky <= 1'b0;
                                irq_status <= 1'b0;
                                pio_rejected <= 1'b0;
                                start_rejected <= 1'b0;
                            end
                        end
                        4'h4: command_word <= write_data;
                        4'h5: if (command_push) begin
                            command_fifo[command_write_ptr] <= command_word;
                            command_last_fifo[command_write_ptr] <= write_data[1];
                            command_write_ptr <= command_write_ptr + 1'b1;
                            renderer_clock_enable <= 1'b1;
                        end
                        4'h7: cmd_addr <= write_data;
                        4'h8: cmd_words <= write_data;
                        4'h9: if (fetch_submit_request) begin
                            if (cmd_addr[2:0] != 3'd0) begin
                                error_sticky <= 1'b1;
                                error_address <= cmd_addr;
                            end else if (cmd_words == 0 || !fetch_submit_ready) begin
                                start_rejected <= 1'b1;
                            end else begin
                                submissions_accepted <= submissions_accepted + 1'b1;
                                renderer_clock_enable <= 1'b1;
                            end
                        end
                        default: begin end
                    endcase
                end
                if (pio_push_request && fetch_busy)
                    pio_rejected <= 1'b1;
                write_addr_valid <= 1'b0;
                write_data_valid <= 1'b0;
                write_response_valid <= 1'b1;
            end

            if (fetch_read_error) begin
                error_sticky <= 1'b1;
                irq_status <= 1'b1;
                error_address <= fetch_read_error_addr;
            end
            // ZZ9000 presents completed color targets through its RTG path,
            // not RasterIX's display-page swap interface.  A swap is therefore
            // acknowledged below only to keep the renderer from stalling, then
            // reported to software as an unsupported stream operation.
            if (rix_swap_fb) begin
                error_sticky <= 1'b1;
                irq_status <= 1'b1;
            end
            if (fetch_submission_done)
                completion_pending <= 1'b1;
            if (completion_ack) begin
                completion_pending <= 1'b0;
                irq_status <= 1'b1;
                submissions_completed <= submissions_completed + 1'b1;
            end

            if (soft_reset_pending && engine_idle) begin
                soft_reset_pending <= 1'b0;
                soft_reset_active <= 1'b1;
                command_write_ptr <= 4'd0;
                command_read_ptr <= 4'd0;
                command_count <= 5'd0;
            end else begin
                soft_reset_active <= 1'b0;
            end

            if (command_pop)
                command_read_ptr <= command_read_ptr + 1'b1;
            case ({command_push, command_pop})
                2'b10: command_count <= command_count + 1'b1;
                2'b01: command_count <= command_count - 1'b1;
                default: begin end
            endcase

            case ({m_axi_awvalid && m_axi_awready, m_axi_bvalid && m_axi_bready})
                2'b10: write_transactions <= write_transactions + 1'b1;
                2'b01: write_transactions <= write_transactions - 1'b1;
                default: begin end
            endcase
            case ({m_axi_arvalid && m_axi_arready, m_axi_rvalid && m_axi_rready && m_axi_rlast})
                2'b10: read_transactions <= read_transactions + 1'b1;
                2'b01: read_transactions <= read_transactions - 1'b1;
                default: begin end
            endcase

            if (!enable_requested && engine_idle && !soft_reset_pending &&
                !command_push && !fetch_submit_fire)
                renderer_clock_enable <= 1'b0;
        end
    end

    zz9k_cmd_fetcher command_fetcher (
        .aclk(m_axi_aclk),
        .aresetn(m_axi_aresetn),
        .submit_valid(fetch_submit_fire),
        .submit_ready(fetch_submit_ready),
        .submit_addr(cmd_addr),
        .submit_words(cmd_words),
        .busy(fetch_busy),
        .m_cmd_axis_tvalid(fetch_command_valid),
        .m_cmd_axis_tready(fetch_command_ready),
        .m_cmd_axis_tlast(fetch_command_last),
        .m_cmd_axis_tdata(fetch_command_data),
        .submission_done(fetch_submission_done),
        .completion_ack(completion_ack),
        .read_error(fetch_read_error),
        .read_error_addr(fetch_read_error_addr),
        .m_axi_araddr(m_cmd_axi_araddr),
        .m_axi_arlen(m_cmd_axi_arlen),
        .m_axi_arsize(m_cmd_axi_arsize),
        .m_axi_arburst(m_cmd_axi_arburst),
        .m_axi_arlock(m_cmd_axi_arlock),
        .m_axi_arcache(m_cmd_axi_arcache),
        .m_axi_arprot(m_cmd_axi_arprot),
        .m_axi_arvalid(m_cmd_axi_arvalid),
        .m_axi_arready(m_cmd_axi_arready),
        .m_axi_rdata(m_cmd_axi_rdata),
        .m_axi_rresp(m_cmd_axi_rresp),
        .m_axi_rlast(m_cmd_axi_rlast),
        .m_axi_rvalid(m_cmd_axi_rvalid),
        .m_axi_rready(m_cmd_axi_rready)
    );
    assign m_cmd_axi_arid = 4'd0;

`ifdef RASTERIX_VARIANT_EF
    // EF renders RGB565 directly to external DDR.  Its StreamFramebuffer
    // handles the 16-bit color/depth targets, so no IF band configuration is
    // applicable here.
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
        .ID_WIDTH(4),
        .DATA_WIDTH(32),
        .STRB_WIDTH(4),
        .ENABLE_MEMORY_COALESCING(1),
        .RASTERIZER_FLOAT_PRECISION(32),
        .RASTERIZER_FIXPOINT_PRECISION(25),
        .RASTERIZER_ENABLE_FLOAT_INTERPOLATION(0)
    )
`else
    RasterIX_IF #(
        .FRAMEBUFFER_SIZE_IN_PIXEL_LG(`RASTERIX_FRAMEBUFFER_SIZE_IN_PIXEL_LG),
        .FRAMEBUFFER_SUB_PIXEL_WIDTH(5),
        .SUB_PIXEL_CALC_PRECISION(8),
        .FRAMEBUFFER_ENABLE_ALPHA_CHANNEL(0),
        .ENABLE_STENCIL_BUFFER(0),
        .ENABLE_DEPTH_BUFFER(1),
        .TMU_COUNT(`RASTERIX_TMU_COUNT),
        .ENABLE_MIPMAPPING(1),
        .ENABLE_TEXTURE_FILTERING(1),
        .TEXTURE_PAGE_SIZE(2048),
        .ENABLE_EXTERNAL_TEXTURE_MEMORY(1),
        .CACHE_SIZE(8192),
        .ENABLE_FOG(1),
        .MAX_TEXTURE_SIZE(256),
        .ADDR_WIDTH(32),
        .ID_WIDTH(4),
        .DATA_WIDTH(32),
        .STRB_WIDTH(4),
        .RASTERIZER_FLOAT_PRECISION(32),
        .RASTERIZER_FIXPOINT_PRECISION(25),
        .RASTERIZER_ENABLE_FLOAT_INTERPOLATION(0)
    )
`endif
    renderer (
        .aclk(renderer_clock),
        .resetn(renderer_resetn),
        .s_cmd_axis_tvalid(rix_command_valid),
        .s_cmd_axis_tready(rix_command_ready),
        .s_cmd_axis_tlast(rix_command_last),
        .s_cmd_axis_tdata(rix_command_data),
        .m_cmd_resp_axis_tvalid(rix_response_valid),
        .m_cmd_resp_axis_tready(rix_response_ready),
        .m_cmd_resp_axis_tlast(rix_response_last),
        .m_cmd_resp_axis_tdata(rix_response_data),
        .swap_fb(rix_swap_fb),
        .swap_fb_enable_vsync(rix_swap_fb_enable_vsync),
        .fb_addr(rix_fb_addr),
        .fb_size(rix_fb_size),
        .fb_swapped(1'b1),
        .m_axi_awid(rix_m_axi_awid),
        .m_axi_awaddr(rix_m_axi_awaddr),
        .m_axi_awlen(rix_m_axi_awlen),
        .m_axi_awsize(rix_m_axi_awsize),
        .m_axi_awburst(rix_m_axi_awburst),
        .m_axi_awlock(rix_m_axi_awlock),
        .m_axi_awcache(rix_m_axi_awcache),
        .m_axi_awprot(rix_m_axi_awprot),
        .m_axi_awvalid(rix_m_axi_awvalid),
        .m_axi_awready(m_axi_awready),
        .m_axi_wdata(rix_m_axi_wdata),
        .m_axi_wstrb(rix_m_axi_wstrb),
        .m_axi_wlast(rix_m_axi_wlast),
        .m_axi_wvalid(rix_m_axi_wvalid),
        .m_axi_wready(m_axi_wready),
        .m_axi_bid(m_axi_bid),
        .m_axi_bresp(m_axi_bresp),
        .m_axi_bvalid(m_axi_bvalid),
        .m_axi_bready(rix_m_axi_bready),
        .m_axi_arid(rix_m_axi_arid),
        .m_axi_araddr(rix_m_axi_araddr),
        .m_axi_arlen(rix_m_axi_arlen),
        .m_axi_arsize(rix_m_axi_arsize),
        .m_axi_arburst(rix_m_axi_arburst),
        .m_axi_arlock(rix_m_axi_arlock),
        .m_axi_arcache(rix_m_axi_arcache),
        .m_axi_arprot(rix_m_axi_arprot),
        .m_axi_arvalid(rix_m_axi_arvalid),
        .m_axi_arready(m_axi_arready),
        .m_axi_rid(m_axi_rid),
        .m_axi_rdata(m_axi_rdata),
        .m_axi_rresp(m_axi_rresp),
        .m_axi_rlast(m_axi_rlast),
        .m_axi_rvalid(m_axi_rvalid),
        .m_axi_rready(rix_m_axi_rready),
        .perfBusy(renderer_busy),
        .perfTriangleRendering(renderer_triangle_rendering),
        .perfRasterizerStall(renderer_stall)
    );
endmodule
