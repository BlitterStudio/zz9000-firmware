`timescale 1ns/1ps
// Real ZZ9000 RasterIX integration testbench.  The command buffer is an ARM
// DDR image: FrameStreamingCore header/address followed by parser words.
module tb_zz9k_rasterix;
    // Keep low ARM DDR compact while translating MiniGL's physical buffers.
    localparam integer MEM_BYTES = 4 * 1024 * 1024;
    localparam [31:0] CMD_ADDR = 32'h0010_0000;
    localparam [31:0] COLOR_ADDR = 32'h0020_0000;
    localparam [31:0] MINIGL_COLOR_ADDR = 32'h0782_0000;
    localparam [31:0] MINIGL_DEPTH_ADDR = 32'h07b2_0000;
    localparam [31:0] MINIGL_TEXTURE_ADDR = 32'h0038_0000;
    localparam integer WIDTH = 640;
    localparam integer HEIGHT = 480;
    localparam integer COLOR_BYTES = WIDTH * HEIGHT * 2;
    localparam integer DEPTH_BYTES = WIDTH * HEIGHT * 4;
    // RasterIX aligns each external framebuffer transfer to 2048 bytes.
    // MiniGL's fixed ABI reserves complete 3 MiB surfaces, so model those
    // ranges rather than only the 640x480 visible rectangles.
    localparam integer MINIGL_COLOR_MEMORY_BYTES = 3 * 1024 * 1024;
    localparam integer MINIGL_DEPTH_MEMORY_BYTES = 3 * 1024 * 1024;
    localparam integer TEXTURE_POOL_BYTES = 24 * 1024 * 1024;
    // textures.hex: white and lightmap 8x8 pages plus the 256x256 ef-stress
    // block texture (66 pages of 2 KiB at 0x00340000).
    localparam integer TEXTURE_BYTES = 66 * 2048;

    reg clk;
    reg resetn;
    always #5 clk = ~clk;

    reg [15:0] s_axi_awaddr;
    reg [2:0] s_axi_awprot;
    reg s_axi_awvalid;
    wire s_axi_awready;
    reg [31:0] s_axi_wdata;
    reg [3:0] s_axi_wstrb;
    reg s_axi_wvalid;
    wire s_axi_wready;
    wire [1:0] s_axi_bresp;
    wire s_axi_bvalid;
    reg s_axi_bready;
    reg [15:0] s_axi_araddr;
    reg [2:0] s_axi_arprot;
    reg s_axi_arvalid;
    wire s_axi_arready;
    wire [31:0] s_axi_rdata;
    wire [1:0] s_axi_rresp;
    wire s_axi_rvalid;
    reg s_axi_rready;
    reg s_cmd_axis_tvalid;
    wire s_cmd_axis_tready;
    reg s_cmd_axis_tlast;
    reg [31:0] s_cmd_axis_tdata;

    wire [5:0] m_axi_awid;
    wire [31:0] m_axi_awaddr;
    wire [7:0] m_axi_awlen;
    wire [2:0] m_axi_awsize;
    wire [1:0] m_axi_awburst;
    wire m_axi_awlock;
    wire [3:0] m_axi_awcache;
    wire [2:0] m_axi_awprot;
    wire m_axi_awvalid;
    reg m_axi_awready;
    wire [63:0] m_axi_wdata;
    wire [7:0] m_axi_wstrb;
    wire m_axi_wlast;
    wire m_axi_wvalid;
    reg m_axi_wready;
    reg [5:0] m_axi_bid;
    reg [1:0] m_axi_bresp;
    reg m_axi_bvalid;
    wire m_axi_bready;
    wire [5:0] m_axi_arid;
    wire [31:0] m_axi_araddr;
    wire [7:0] m_axi_arlen;
    wire [2:0] m_axi_arsize;
    wire [1:0] m_axi_arburst;
    wire m_axi_arlock;
    wire [3:0] m_axi_arcache;
    wire [2:0] m_axi_arprot;
    wire m_axi_arvalid;
    reg m_axi_arready;
    reg [5:0] m_axi_rid;
    reg [63:0] m_axi_rdata;
    reg [1:0] m_axi_rresp;
    reg m_axi_rlast;
    reg m_axi_rvalid;
    wire m_axi_rready;

    wire [3:0] m_cmd_axi_arid;
    wire [31:0] m_cmd_axi_araddr;
    wire [7:0] m_cmd_axi_arlen;
    wire [2:0] m_cmd_axi_arsize;
    wire [1:0] m_cmd_axi_arburst;
    wire m_cmd_axi_arlock;
    wire [3:0] m_cmd_axi_arcache;
    wire [2:0] m_cmd_axi_arprot;
    wire m_cmd_axi_arvalid;
    reg m_cmd_axi_arready;
    reg [3:0] m_cmd_axi_rid;
    reg [63:0] m_cmd_axi_rdata;
    reg [1:0] m_cmd_axi_rresp;
    reg m_cmd_axi_rlast;
    reg m_cmd_axi_rvalid;
    wire m_cmd_axi_rready;
`ifdef RASTERIX_VARIANT_EF
    // EF has no command-read master (commands arrive on s_cmd_axis).  Tie the
    // legacy model's master side idle so no X reaches the memory models.
    assign m_cmd_axi_arid = 0;
    assign m_cmd_axi_araddr = 0;
    assign m_cmd_axi_arlen = 0;
    assign m_cmd_axi_arsize = 0;
    assign m_cmd_axi_arburst = 0;
    assign m_cmd_axi_arlock = 0;
    assign m_cmd_axi_arcache = 0;
    assign m_cmd_axi_arprot = 0;
    assign m_cmd_axi_arvalid = 0;
    assign m_cmd_axi_rready = 1;
`endif

    zz9k_rasterix dut (
        .s_axi_aclk(clk), .s_axi_aresetn(resetn),
        .s_axi_awaddr(s_axi_awaddr), .s_axi_awprot(s_axi_awprot),
        .s_axi_awvalid(s_axi_awvalid), .s_axi_awready(s_axi_awready),
        .s_axi_wdata(s_axi_wdata), .s_axi_wstrb(s_axi_wstrb),
        .s_axi_wvalid(s_axi_wvalid), .s_axi_wready(s_axi_wready),
        .s_axi_bresp(s_axi_bresp), .s_axi_bvalid(s_axi_bvalid), .s_axi_bready(s_axi_bready),
        .s_axi_araddr(s_axi_araddr), .s_axi_arprot(s_axi_arprot),
        .s_axi_arvalid(s_axi_arvalid), .s_axi_arready(s_axi_arready),
        .s_axi_rdata(s_axi_rdata), .s_axi_rresp(s_axi_rresp),
        .s_axi_rvalid(s_axi_rvalid), .s_axi_rready(s_axi_rready),
        .m_axi_aclk(clk), .m_axi_aresetn(resetn),
        .s_cmd_axis_tvalid(s_cmd_axis_tvalid), .s_cmd_axis_tready(s_cmd_axis_tready),
        .s_cmd_axis_tlast(s_cmd_axis_tlast), .s_cmd_axis_tdata(s_cmd_axis_tdata),
        .m_axi_awid(m_axi_awid), .m_axi_awaddr(m_axi_awaddr), .m_axi_awlen(m_axi_awlen),
        .m_axi_awsize(m_axi_awsize), .m_axi_awburst(m_axi_awburst), .m_axi_awlock(m_axi_awlock),
        .m_axi_awcache(m_axi_awcache), .m_axi_awprot(m_axi_awprot), .m_axi_awvalid(m_axi_awvalid),
        .m_axi_awready(m_axi_awready), .m_axi_wdata(m_axi_wdata), .m_axi_wstrb(m_axi_wstrb),
        .m_axi_wlast(m_axi_wlast), .m_axi_wvalid(m_axi_wvalid), .m_axi_wready(m_axi_wready),
        .m_axi_bid(m_axi_bid), .m_axi_bresp(m_axi_bresp), .m_axi_bvalid(m_axi_bvalid),
        .m_axi_bready(m_axi_bready), .m_axi_arid(m_axi_arid), .m_axi_araddr(m_axi_araddr),
        .m_axi_arlen(m_axi_arlen), .m_axi_arsize(m_axi_arsize), .m_axi_arburst(m_axi_arburst),
        .m_axi_arlock(m_axi_arlock), .m_axi_arcache(m_axi_arcache), .m_axi_arprot(m_axi_arprot),
        .m_axi_arvalid(m_axi_arvalid), .m_axi_arready(m_axi_arready), .m_axi_rid(m_axi_rid),
        .m_axi_rdata(m_axi_rdata), .m_axi_rresp(m_axi_rresp), .m_axi_rlast(m_axi_rlast),
        .m_axi_rvalid(m_axi_rvalid), .m_axi_rready(m_axi_rready)
    );

    reg [7:0] memory [0:MEM_BYTES-1];
    reg [7:0] color_memory [0:MINIGL_COLOR_MEMORY_BYTES-1];
    reg [7:0] depth_memory [0:MINIGL_DEPTH_MEMORY_BYTES-1];
    reg [7:0] texture_memory [0:TEXTURE_POOL_BYTES-1];
    reg [31:0] command_words [0:262143];
    reg [7:0] texture_bytes [0:TEXTURE_BYTES-1];
    integer i;
    integer latency;
    integer cycle_count;
    integer trace_pipeline;
    reg [31:0] cmd_read_addr;
    integer cmd_read_left;
    integer cmd_read_delay;
    reg cmd_read_active;
    reg [31:0] render_read_addr;
    integer render_read_left;
    integer render_read_delay;
    reg render_read_active;
    reg [5:0] render_read_id;
    reg [2:0] render_read_size;
    reg [31:0] write_addr;
    integer write_left;
    reg write_active;
    reg write_error;
    reg [5:0] write_id;
    // Shared HP-port model (SHARED=1): accepted renderer read addresses wait
    // in one bounded queue that feeds ONE response stream.  Beats of a burst
    // are never interleaved with another burst, so a crossbar port that
    // withholds RREADY blocks every response queued behind it -- the PS7 HP
    // AFI/interconnect head-of-line hazard.  RAND_SEED=n (n != 0) adds
    // 0..RAND_LATENCY extra cycles before every beat and serves next the
    // oldest request carrying the ID of a randomly chosen queued request, so
    // responses with different IDs are reordered as AXI permits while
    // same-ID order is kept.  SHARED=0 (default) keeps the one-transaction
    // separate model.
    integer shared_channel;
    integer rand_seed;
    integer rand_latency;
    integer rand_pick;
    // Seeded LCG owned by the shared read model's process ($urandom streams
    // are per process, so a seed applied in the initial block would not
    // reach it).
    reg [31:0] rand_state;
    integer shared_sel;
    integer shared_served;
    integer shared_reordered;
    integer shared_pop;
    integer shared_k;
    reg [31:0] shared_q_addr [0:31];
    integer shared_q_len [0:31];
    reg [5:0] shared_q_id [0:31];
    reg [2:0] shared_q_size [0:31];
    integer shared_q_count;
    reg shared_active;
    reg [31:0] shared_addr;
    integer shared_left;
    integer shared_delay;
    reg [5:0] shared_id;
    reg [2:0] shared_size;

    function [15:0] rand_next;
        input integer dummy;
        begin
            rand_state = rand_state * 32'd1664525 + 32'd1013904223;
            rand_next = rand_state[31:16];
        end
    endfunction

    function integer beat_latency;
        input integer dummy;
        begin
            beat_latency = latency;
            if (rand_seed != 0)
                beat_latency = latency + (rand_next(0) % (rand_latency + 1));
        end
    endfunction

    function [7:0] byte_at;
        input [31:0] address;
        begin
            if ((address >= MINIGL_COLOR_ADDR) && (address < MINIGL_COLOR_ADDR + MINIGL_COLOR_MEMORY_BYTES))
                byte_at = color_memory[address - MINIGL_COLOR_ADDR];
            else if ((address >= MINIGL_DEPTH_ADDR) && (address < MINIGL_DEPTH_ADDR + MINIGL_DEPTH_MEMORY_BYTES))
                byte_at = depth_memory[address - MINIGL_DEPTH_ADDR];
            else if ((address >= MINIGL_TEXTURE_ADDR) && (address < MINIGL_TEXTURE_ADDR + TEXTURE_POOL_BYTES))
                byte_at = texture_memory[address - MINIGL_TEXTURE_ADDR];
            else if (address < MEM_BYTES)
                byte_at = memory[address];
            else
                byte_at = 8'h00;
        end
    endfunction
    function address_mapped;
        input [31:0] address;
        begin
            address_mapped = ((address >= MINIGL_COLOR_ADDR) &&
                              (address < MINIGL_COLOR_ADDR + MINIGL_COLOR_MEMORY_BYTES)) ||
                             ((address >= MINIGL_DEPTH_ADDR) &&
                              (address < MINIGL_DEPTH_ADDR + MINIGL_DEPTH_MEMORY_BYTES)) ||
                             ((address >= MINIGL_TEXTURE_ADDR) &&
                              (address < MINIGL_TEXTURE_ADDR + TEXTURE_POOL_BYTES)) ||
                             (address < MEM_BYTES);
        end
    endfunction

    function write_word_mapped;
        input [31:0] address;
        input [3:0] strobe;
        begin
            write_word_mapped = ((!strobe[0] || address_mapped(address)) &&
                                 (!strobe[1] || address_mapped(address + 1)) &&
                                 (!strobe[2] || address_mapped(address + 2)) &&
                                 (!strobe[3] || address_mapped(address + 3)));
        end
    endfunction

    task write_byte;
        input [31:0] address;
        input [7:0] value;
        begin
            if ((address >= MINIGL_COLOR_ADDR) && (address < MINIGL_COLOR_ADDR + MINIGL_COLOR_MEMORY_BYTES))
                color_memory[address - MINIGL_COLOR_ADDR] = value;
            else if ((address >= MINIGL_DEPTH_ADDR) && (address < MINIGL_DEPTH_ADDR + MINIGL_DEPTH_MEMORY_BYTES))
                depth_memory[address - MINIGL_DEPTH_ADDR] = value;
            else if ((address >= MINIGL_TEXTURE_ADDR) && (address < MINIGL_TEXTURE_ADDR + TEXTURE_POOL_BYTES))
                texture_memory[address - MINIGL_TEXTURE_ADDR] = value;
            else if (address < MEM_BYTES)
                memory[address] = value;
        end
    endtask
    function [31:0] word_at;
        input [31:0] address;
        begin
            word_at = {byte_at(address + 3), byte_at(address + 2), byte_at(address + 1), byte_at(address)};
        end
    endfunction
    function [63:0] dword_at;
        input [31:0] address;
        begin
            dword_at = {word_at(address + 4), word_at(address)};
        end
    endfunction

    // The model accepts one transaction on each read path and one renderer
    // write burst.  READY is periodically withdrawn to exercise all wrappers.
    // In shared mode one bounded queue serves both read masters so address
    // requests stay realistic while the data stream stays strictly ordered.
    always @* begin
        if (shared_channel) begin
            m_cmd_axi_arready = 0;
            m_axi_arready = (shared_q_count <= 26) && ((cycle_count % 5) != 0);
        end else begin
            m_cmd_axi_arready = !cmd_read_active && !m_cmd_axi_rvalid && ((cycle_count % 7) != 0);
            m_axi_arready = !render_read_active && !m_axi_rvalid && ((cycle_count % 5) != 0);
        end
        m_axi_awready = !write_active && !m_axi_bvalid && ((cycle_count % 6) != 0);
        m_axi_wready = write_active && ((cycle_count % 4) != 0);
    end

    always @(posedge clk) begin
        cycle_count <= cycle_count + 1;
`ifndef RASTERIX_VARIANT_EF
        if (trace_pipeline && (((cycle_count >= 115000) && (cycle_count <= 125000)) ||
            ((cycle_count != 0) && ((cycle_count % 100000) == 0))))
            $display("PIPE cycle=%0d rr=%b/%b sem=%b/%b lock=%0d attr=%b/%b,%b/%b pixready=%b bc2=%b/%b ar=%b/%b/%b alrp=%b/%b tmu=%b/%b/%b/%b frag=%b/%b concat=%b/%b full=%b empty=%b pfp=%b/%b out=%b/%b c=%b/%b/%b d=%b/%b/%b s=%b/%b/%b semcnt=%0d",
                     cycle_count,
                     dut.renderer.rixif.graphicCore.rasterizer_tvalid,
                     dut.renderer.rixif.graphicCore.rasterizer_tready,
                     dut.renderer.rixif.graphicCore.semaphoreRasterizerValid,
                     dut.renderer.rixif.graphicCore.semaphoreRasterizerReady,
                     dut.renderer.rixif.graphicCore.rasterizer_tpixel,
                     dut.renderer.rixif.graphicCore.bc_attr_valid[0],
                     dut.renderer.rixif.graphicCore.bc_attr_ready[0],
                     dut.renderer.rixif.graphicCore.bc_attr_valid[1],
                     dut.renderer.rixif.graphicCore.bc_attr_ready[1],
                     dut.renderer.rixif.graphicCore.bc_pixel_ready,
                     dut.renderer.rixif.graphicCore.bc2_valid,
                     dut.renderer.rixif.graphicCore.bc2_ready,
                     dut.renderer.rixif.graphicCore.m_color_arvalid,
                     dut.renderer.rixif.graphicCore.m_depth_arvalid,
                     dut.renderer.rixif.graphicCore.m_stencil_arvalid,
                     dut.renderer.rixif.graphicCore.alrp_tvalid,
                     dut.renderer.rixif.graphicCore.alrp_tready,
                     dut.renderer.rixif.graphicCore.tr0_valid,
                     dut.renderer.rixif.graphicCore.tr0_ready,
                     dut.renderer.rixif.graphicCore.tr0_texel_valid,
                     dut.renderer.rixif.graphicCore.tr0_texel_ready,
                     dut.renderer.rixif.graphicCore.framebuffer_tvalid,
                     dut.renderer.rixif.graphicCore.framebuffer_tready,
                     dut.renderer.rixif.graphicCore.fragment_stream_out_tvalid,
                     dut.renderer.rixif.graphicCore.fragment_stream_out_tready,
                     dut.renderer.rixif.graphicCore.streamConcatFifo.stream_full,
                     dut.renderer.rixif.graphicCore.streamConcatFifo.stream_empty,
                     dut.renderer.rixif.graphicCore.framebuffer_pfp_wvalid,
                     dut.renderer.rixif.graphicCore.framebuffer_pfp_wready,
                     dut.renderer.rixif.graphicCore.framebuffer_bc_wvalid,
                     dut.renderer.rixif.graphicCore.framebuffer_bc_wfull,
                     dut.renderer.rixif.graphicCore.m_color_wvalid,
                     dut.renderer.rixif.graphicCore.m_color_rvalid,
                     dut.renderer.rixif.graphicCore.m_color_rready,
                     dut.renderer.rixif.graphicCore.m_depth_wvalid,
                     dut.renderer.rixif.graphicCore.m_depth_rvalid,
                     dut.renderer.rixif.graphicCore.m_depth_rready,
                     dut.renderer.rixif.graphicCore.m_stencil_wvalid,
                     dut.renderer.rixif.graphicCore.m_stencil_rvalid,
                     dut.renderer.rixif.graphicCore.m_stencil_rready,
                     dut.renderer.rixif.graphicCore.concatSem.valuesCounter);
        if (trace_pipeline && (cycle_count != 0) && ((cycle_count % 10000) == 0))
            $display("RASTER cycle=%0d run=%b state=%0d x=%0d y=%0d ys=%0d ye=%0d line=%0d bb=%0d,%0d",
                     cycle_count, dut.renderer.rixif.graphicCore.rasterizer.rasterizerRunning,
                     dut.renderer.rixif.graphicCore.rasterizer.rasterizerState,
                     dut.renderer.rixif.graphicCore.rasterizer.x,
                     dut.renderer.rixif.graphicCore.rasterizer.y,
                     dut.renderer.rixif.graphicCore.rasterizer.yScreen,
                     dut.renderer.rixif.graphicCore.rasterizer.yScreenEnd,
                     dut.renderer.rixif.graphicCore.rasterizer.lineBBStart,
                     dut.renderer.rixif.graphicCore.rasterizer.bbStart,
                     dut.renderer.rixif.graphicCore.rasterizer.bbEnd);
        if (dut.renderer.rixif.graphicCore.m_color_wvalid &&
            dut.renderer.rixif.graphicCore.m_color_waddr == 48407)
            $display("S4_X_WRITE cycle=%0d xy=%0d,%0d color=%04x pfp=%04x fragment=%h",
                     cycle_count,
                     dut.renderer.rixif.graphicCore.m_color_wscreenPosX,
                     dut.renderer.rixif.graphicCore.m_color_wscreenPosY,
                     dut.renderer.rixif.graphicCore.m_color_wdata,
                     dut.renderer.rixif.graphicCore.framebuffer_color_pfp_wdata,
                     dut.renderer.rixif.graphicCore.framebuffer_tfragmentColor);
        if (dut.renderer.rixif.graphicCore.framebuffer_pfp_wvalid &&
            (^dut.renderer.rixif.graphicCore.framebuffer_color_pfp_wdata === 1'bx))
            $display("PFP_X cycle=%0d addr=%0d xy=%0d,%0d color=%04x fragment=%h",
                     cycle_count,
                     dut.renderer.rixif.graphicCore.framebuffer_pfp_waddr,
                     dut.renderer.rixif.graphicCore.framebuffer_pfp_wscreenPosX,
                     dut.renderer.rixif.graphicCore.framebuffer_pfp_wscreenPosY,
                     dut.renderer.rixif.graphicCore.framebuffer_color_pfp_wdata,
                     dut.renderer.rixif.graphicCore.framebuffer_tfragmentColor);
        if (dut.renderer.rixif.graphicCore.framebuffer_tvalid &&
            (^dut.renderer.rixif.graphicCore.framebuffer_tfragmentColor === 1'bx))
            $display("PIXEL_X cycle=%0d index=%0d xy=%0d,%0d color=%h attr_color=%h uv=%h,%h",
                     cycle_count,
                     dut.renderer.rixif.graphicCore.framebuffer_tindex,
                     dut.renderer.rixif.graphicCore.framebuffer_tscreenPosX,
                     dut.renderer.rixif.graphicCore.framebuffer_tscreenPosY,
                     dut.renderer.rixif.graphicCore.framebuffer_tfragmentColor,
                     {dut.renderer.rixif.graphicCore.alrp_tcolor_r,
                      dut.renderer.rixif.graphicCore.alrp_tcolor_g,
                      dut.renderer.rixif.graphicCore.alrp_tcolor_b,
                      dut.renderer.rixif.graphicCore.alrp_tcolor_a},
                     dut.renderer.rixif.graphicCore.alrp_ttexture0_s,
                     dut.renderer.rixif.graphicCore.alrp_ttexture0_t);
        if (dut.renderer.rixif.graphicCore.pixelPipeline.tmu0.step2_valid &&
            (^{dut.renderer.rixif.graphicCore.pixelPipeline.tmu0.step2_texel00,
               dut.renderer.rixif.graphicCore.pixelPipeline.tmu0.step2_texel01,
               dut.renderer.rixif.graphicCore.pixelPipeline.tmu0.step2_texel10,
               dut.renderer.rixif.graphicCore.pixelPipeline.tmu0.step2_texel11} === 1'bx))
            $display("TMU0_TEXEL_X cycle=%0d texels=%h,%h,%h,%h",
                     cycle_count,
                     dut.renderer.rixif.graphicCore.pixelPipeline.tmu0.step2_texel00,
                     dut.renderer.rixif.graphicCore.pixelPipeline.tmu0.step2_texel01,
                     dut.renderer.rixif.graphicCore.pixelPipeline.tmu0.step2_texel10,
                     dut.renderer.rixif.graphicCore.pixelPipeline.tmu0.step2_texel11);
        if (dut.renderer.rixif.graphicCore.pixelPipeline.tmu0.step4_valid &&
            (^dut.renderer.rixif.graphicCore.pixelPipeline.tmu0.step4_texel === 1'bx))
            $display("TMU0_FILTER_X cycle=%0d texel=%h", cycle_count,
                     dut.renderer.rixif.graphicCore.pixelPipeline.tmu0.step4_texel);
        if (dut.renderer.rixif.graphicCore.pixelPipeline.tmu0.step5_valid &&
            (^dut.renderer.rixif.graphicCore.pixelPipeline.tmu0.step5_texel === 1'bx))
            $display("TMU0_TEXENV_X cycle=%0d texel=%h", cycle_count,
                     dut.renderer.rixif.graphicCore.pixelPipeline.tmu0.step5_texel);
`endif

        if (!shared_channel) begin : separate_read_models
        if (m_cmd_axi_arvalid && m_cmd_axi_arready) begin
            if (m_cmd_axi_arsize != 3'b011 || m_cmd_axi_arburst != 2'b01)
                $fatal(1, "bad command AXI read");
            cmd_read_addr <= m_cmd_axi_araddr;
            cmd_read_left <= m_cmd_axi_arlen + 1;
            cmd_read_delay <= latency;
            cmd_read_active <= 1;
        end
        if (cmd_read_active && !m_cmd_axi_rvalid) begin
            if (cmd_read_delay != 0) cmd_read_delay <= cmd_read_delay - 1;
            else begin
                m_cmd_axi_rvalid <= 1;
                m_cmd_axi_rdata <= dword_at(cmd_read_addr);
                m_cmd_axi_rid <= 0;
                m_cmd_axi_rresp <= 0;
                m_cmd_axi_rlast <= (cmd_read_left == 1);
            end
        end
        if (m_cmd_axi_rvalid && m_cmd_axi_rready) begin
            m_cmd_axi_rvalid <= 0;
            if (cmd_read_left == 1) begin
                cmd_read_active <= 0;
                m_cmd_axi_rlast <= 0;
            end else begin
                cmd_read_addr <= cmd_read_addr + 8;
                cmd_read_left <= cmd_read_left - 1;
                cmd_read_delay <= latency;
            end
        end

        if (m_axi_arvalid && m_axi_arready) begin
            if (trace_pipeline)
                $display("RENDER_AR cycle=%0d addr=%08x len=%0d id=%0d size=%0d",
                         cycle_count, m_axi_araddr, m_axi_arlen, m_axi_arid, m_axi_arsize);
            render_read_addr <= m_axi_araddr;
            render_read_left <= m_axi_arlen + 1;
            render_read_delay <= latency;
            render_read_active <= 1;
            render_read_id <= m_axi_arid;
            render_read_size <= m_axi_arsize;
        end
        if (render_read_active && !m_axi_rvalid) begin
            if (render_read_delay != 0) render_read_delay <= render_read_delay - 1;
            else begin
                m_axi_rvalid <= 1;
                m_axi_rdata <= dword_at(render_read_addr);
                m_axi_rid <= render_read_id;
                m_axi_rresp <= address_mapped(render_read_addr) ? 0 : 2;
                if (!address_mapped(render_read_addr))
                    $error("renderer AXI read outside sparse DDR: %08x", render_read_addr);
                m_axi_rlast <= (render_read_left == 1);
            end
        end
        if (m_axi_rvalid && m_axi_rready) begin
            if (trace_pipeline && (m_axi_rlast || render_read_left == 1))
                $display("RENDER_R_LAST cycle=%0d addr=%08x left=%0d id=%0d resp=%0d last=%b",
                         cycle_count, render_read_addr, render_read_left, m_axi_rid,
                         m_axi_rresp, m_axi_rlast);
            m_axi_rvalid <= 0;
            if (render_read_left == 1) begin
                render_read_active <= 0;
                m_axi_rlast <= 0;
            end else begin
                render_read_addr <= render_read_addr + (1 << render_read_size);
                render_read_left <= render_read_left - 1;
                render_read_delay <= latency;
            end
        end
        end else begin : shared_read_model
            // Select the next transaction once the previous one has fully
            // returned; selecting pops it from the queue.  A same-cycle AR is
            // appended behind the compacted queue (its write comes last, so
            // it overrides the shift of the vacated tail slot).
            shared_pop = 0;
            if (!shared_active && !m_axi_rvalid && (shared_q_count != 0)) begin
                shared_sel = 0;
                if (rand_seed != 0) begin
                    rand_pick = rand_next(0) % shared_q_count;
                    while (shared_q_id[shared_sel] != shared_q_id[rand_pick])
                        shared_sel = shared_sel + 1;
                end
                shared_active <= 1'b1;
                shared_addr <= shared_q_addr[shared_sel];
                shared_left <= shared_q_len[shared_sel];
                shared_id <= shared_q_id[shared_sel];
                shared_size <= shared_q_size[shared_sel];
                shared_delay <= beat_latency(0);
                for (shared_k = shared_sel; shared_k < 31; shared_k = shared_k + 1) begin
                    shared_q_addr[shared_k] <= shared_q_addr[shared_k + 1];
                    shared_q_len[shared_k] <= shared_q_len[shared_k + 1];
                    shared_q_id[shared_k] <= shared_q_id[shared_k + 1];
                    shared_q_size[shared_k] <= shared_q_size[shared_k + 1];
                end
                shared_pop = 1;
                shared_served = shared_served + 1;
                if (shared_sel != 0) shared_reordered = shared_reordered + 1;
            end else if (shared_active && !m_axi_rvalid) begin
                if (shared_delay != 0) shared_delay <= shared_delay - 1;
                else begin
                    m_axi_rvalid <= 1;
                    m_axi_rdata <= dword_at(shared_addr);
                    m_axi_rid <= shared_id;
                    m_axi_rresp <= address_mapped(shared_addr) ? 0 : 2;
                    if (!address_mapped(shared_addr))
                        $error("renderer AXI read outside sparse DDR: %08x", shared_addr);
                    m_axi_rlast <= (shared_left == 1);
                end
            end

            if (m_axi_arvalid && m_axi_arready) begin
                if (trace_pipeline)
                    $display("RENDER_AR cycle=%0d addr=%08x len=%0d id=%0d size=%0d",
                             cycle_count, m_axi_araddr, m_axi_arlen, m_axi_arid, m_axi_arsize);
                shared_q_addr[shared_q_count - shared_pop] <= m_axi_araddr;
                shared_q_len[shared_q_count - shared_pop] <= m_axi_arlen + 1;
                shared_q_id[shared_q_count - shared_pop] <= m_axi_arid;
                shared_q_size[shared_q_count - shared_pop] <= m_axi_arsize;
            end
            shared_q_count <= shared_q_count - shared_pop + ((m_axi_arvalid && m_axi_arready) ? 1 : 0);

            if (m_axi_rvalid && m_axi_rready) begin
                m_axi_rvalid <= 0;
                shared_addr <= shared_addr + (1 << shared_size);
                shared_left <= shared_left - 1;
                if (shared_left == 1) begin
                    m_axi_rlast <= 0;
                    shared_active <= 1'b0;
                end else begin
                    shared_delay <= beat_latency(0);
                end
            end
        end
        if (trace_pipeline && m_axi_rvalid && !m_axi_rready && ((cycle_count % 10000) == 0))
            $display("RENDER_R_BACKPRESSURE cycle=%0d addr=%08x left=%0d id=%0d resp=%0d last=%b",
                     cycle_count, render_read_addr, render_read_left, m_axi_rid,
                     m_axi_rresp, m_axi_rlast);

        if (m_axi_awvalid && m_axi_awready) begin
            write_active <= 1;
            write_addr <= m_axi_awaddr;
            write_left <= m_axi_awlen + 1;
            write_error <= 0;
            write_id <= m_axi_awid;
        end
        if (m_axi_wvalid && m_axi_wready) begin
            if (!write_word_mapped(write_addr, m_axi_wstrb[3:0]) ||
                !write_word_mapped(write_addr + 4, m_axi_wstrb[7:4])) begin
                $error("renderer AXI write outside sparse DDR: %08x strb=%x",
                       write_addr, m_axi_wstrb);
                write_error <= 1;
            end
            if (m_axi_wstrb[0]) write_byte(write_addr, m_axi_wdata[7:0]);
            if (m_axi_wstrb[1]) write_byte(write_addr + 1, m_axi_wdata[15:8]);
            if (m_axi_wstrb[2]) write_byte(write_addr + 2, m_axi_wdata[23:16]);
            if (m_axi_wstrb[3]) write_byte(write_addr + 3, m_axi_wdata[31:24]);
            if (m_axi_wstrb[4]) write_byte(write_addr + 4, m_axi_wdata[39:32]);
            if (m_axi_wstrb[5]) write_byte(write_addr + 5, m_axi_wdata[47:40]);
            if (m_axi_wstrb[6]) write_byte(write_addr + 6, m_axi_wdata[55:48]);
            if (m_axi_wstrb[7]) write_byte(write_addr + 7, m_axi_wdata[63:56]);
            write_addr <= write_addr + (1 << m_axi_awsize);
            write_left <= write_left - 1;
            if (m_axi_wlast) begin
                if (write_left != 1) $fatal(1, "early renderer WLAST left=%0d addr=%08x awlen=%0d", write_left, write_addr, m_axi_awlen);
                write_active <= 0;
                m_axi_bvalid <= 1;
                m_axi_bid <= write_id;
                m_axi_bresp <= (write_error || !write_word_mapped(write_addr, m_axi_wstrb[3:0]) || !write_word_mapped(write_addr + 4, m_axi_wstrb[7:4])) ? 2 : 0;
            end
        end
        if (m_axi_bvalid && m_axi_bready) m_axi_bvalid <= 0;
    end

`ifdef RASTERIX_VARIANT_EF
    // Memory deadlock evidence.  Every accepted renderer read address goes
    // into a 48-entry history (TRACE_AXI=2 also prints it live).  When the
    // renderer is busy and no handshake on any memory channel or the command
    // stream has happened for DEADLOCK_LIMIT cycles, the run dumps which
    // crossbar port owns the presented read beat, the in-order response
    // queue, per-port channel state and the fragment-pipeline joins, then
    // fails instead of polling to the completion timeout.
    integer trace_axi;
    integer deadlock_limit;
    integer idle_cycles;
    integer ar_hist_n;
    integer dump_k;
    reg [5:0] ar_hist_id [0:47];
    reg [31:0] ar_hist_addr [0:47];
    reg [7:0] ar_hist_len [0:47];
    integer ar_hist_cycle [0:47];
    wire memory_progress = (m_axi_arvalid && m_axi_arready) || (m_axi_rvalid && m_axi_rready) ||
                           (m_axi_awvalid && m_axi_awready) || (m_axi_wvalid && m_axi_wready) ||
                           (m_axi_bvalid && m_axi_bready) || (s_cmd_axis_tvalid && s_cmd_axis_tready);
    task dump_memory_state;
        begin
            $display("MEMSTATE cycle=%0d idle_for=%0d counters ar=%0d r=%0d aw=%0d w=%0d b=%0d",
                     cycle_count, idle_cycles, dut.dbg_ar_count, dut.dbg_r_count,
                     dut.dbg_aw_count, dut.dbg_w_count, dut.dbg_b_count);
            $display("MEMSTATE m_axi ar=%b/%b r=%b/%b rid=%0d (xbar port %0d) rlast=%b aw=%b/%b w=%b/%b b=%b/%b tb_write_active=%b",
                     m_axi_arvalid, m_axi_arready, m_axi_rvalid, m_axi_rready, m_axi_rid, m_axi_rid[5:3],
                     m_axi_rlast, m_axi_awvalid, m_axi_awready, m_axi_wvalid, m_axi_wready,
                     m_axi_bvalid, m_axi_bready, write_active);
            $display("MEMSTATE xbar ports[5:0] ar=%b/%b r=%b/%b aw=%b/%b w=%b/%b b=%b/%b",
                     dut.renderer.xbar_axi_arvalid, dut.renderer.xbar_axi_arready,
                     dut.renderer.xbar_axi_rvalid, dut.renderer.xbar_axi_rready,
                     dut.renderer.xbar_axi_awvalid, dut.renderer.xbar_axi_awready,
                     dut.renderer.xbar_axi_wvalid, dut.renderer.xbar_axi_wready,
                     dut.renderer.xbar_axi_bvalid, dut.renderer.xbar_axi_bready);
            if (shared_channel) begin
                $display("MEMSTATE shared response engine active=%b id=%0d (port %0d) addr=%08x beats_left=%0d queued=%0d",
                         shared_active, shared_id, shared_id[5:3], shared_addr, shared_left, shared_q_count);
                for (dump_k = 0; dump_k < shared_q_count; dump_k = dump_k + 1)
                    $display("MEMSTATE   queue[%0d] id=%0d (port %0d) addr=%08x beats=%0d", dump_k,
                             shared_q_id[dump_k], shared_q_id[dump_k] >> 3,
                             shared_q_addr[dump_k], shared_q_len[dump_k]);
            end
            for (dump_k = (ar_hist_n > 48) ? ar_hist_n - 48 : 0; dump_k < ar_hist_n; dump_k = dump_k + 1)
                $display("MEMSTATE   ar_history cycle=%0d port=%0d id=%0d addr=%08x len=%0d",
                         ar_hist_cycle[dump_k % 48], ar_hist_id[dump_k % 48] >> 3,
                         ar_hist_id[dump_k % 48], ar_hist_addr[dump_k % 48], ar_hist_len[dump_k % 48]);
            $display("MEMSTATE pipeline sem=%b/%b semcount=%0d bc2=%b/%b color_ar=%b/%b depth_ar=%b/%b tmu_req=%b/%b tmu_texel=%b/%b frag=%b/%b color_r=%b/%b depth_r=%b/%b concat_out=%b/%b concat_full=%b concat_empty=%b color_w=%b/%b depth_w=%b/%b",
                     dut.renderer.rixef.graphicCore.semaphoreRasterizerValid,
                     dut.renderer.rixef.graphicCore.semaphoreRasterizerReady,
                     dut.renderer.rixef.graphicCore.concatSem.valuesCounter,
                     dut.renderer.rixef.graphicCore.bc2_valid,
                     dut.renderer.rixef.graphicCore.bc2_ready,
                     dut.renderer.rixef.graphicCore.m_color_arvalid,
                     dut.renderer.rixef.graphicCore.m_color_arready,
                     dut.renderer.rixef.graphicCore.m_depth_arvalid,
                     dut.renderer.rixef.graphicCore.m_depth_arready,
                     dut.renderer.rixef.graphicCore.tr0_valid,
                     dut.renderer.rixef.graphicCore.tr0_ready,
                     dut.renderer.rixef.graphicCore.tr0_texel_valid,
                     dut.renderer.rixef.graphicCore.tr0_texel_ready,
                     dut.renderer.rixef.graphicCore.framebuffer_tvalid,
                     dut.renderer.rixef.graphicCore.framebuffer_tready,
                     dut.renderer.rixef.graphicCore.m_color_rvalid,
                     dut.renderer.rixef.graphicCore.m_color_rready,
                     dut.renderer.rixef.graphicCore.m_depth_rvalid,
                     dut.renderer.rixef.graphicCore.m_depth_rready,
                     dut.renderer.rixef.graphicCore.fragment_stream_out_tvalid,
                     dut.renderer.rixef.graphicCore.fragment_stream_out_tready,
                     dut.renderer.rixef.graphicCore.streamConcatFifo.stream_full,
                     dut.renderer.rixef.graphicCore.streamConcatFifo.stream_empty,
                     dut.renderer.rixef.graphicCore.m_color_wvalid,
                     dut.renderer.rixef.graphicCore.m_color_wready,
                     dut.renderer.rixef.graphicCore.m_depth_wvalid,
                     dut.renderer.rixef.graphicCore.m_depth_wready);
        end
    endtask
    always @(posedge clk) begin
        if (!resetn) begin
            idle_cycles <= 0;
            ar_hist_n <= 0;
        end else begin
            if (m_axi_arvalid && m_axi_arready) begin
                ar_hist_id[ar_hist_n % 48] <= m_axi_arid;
                ar_hist_addr[ar_hist_n % 48] <= m_axi_araddr;
                ar_hist_len[ar_hist_n % 48] <= m_axi_arlen;
                ar_hist_cycle[ar_hist_n % 48] <= cycle_count;
                ar_hist_n <= ar_hist_n + 1;
                if (trace_axi > 1)
                    $display("AXI_AR cycle=%0d port=%0d id=%0d addr=%08x len=%0d",
                             cycle_count, m_axi_arid[5:3], m_axi_arid, m_axi_araddr, m_axi_arlen);
            end
            if (^{m_axi_arvalid, m_axi_arready, m_axi_rvalid, m_axi_rready,
                  m_axi_awvalid, m_axi_awready, m_axi_wvalid, m_axi_wready,
                  m_axi_bvalid, m_axi_bready} === 1'bx)
                $fatal(1, "unknown renderer AXI handshake at cycle %0d", cycle_count);
            if (memory_progress || !dut.renderer.perfBusy) idle_cycles <= 0;
            else idle_cycles <= idle_cycles + 1;
            if (trace_axi && (cycle_count % 250000) == 0)
                $display("HEARTBEAT cycle=%0d busy=%b ar=%0d r=%0d aw=%0d w=%0d b=%0d", cycle_count,
                         dut.renderer.perfBusy, dut.dbg_ar_count, dut.dbg_r_count,
                         dut.dbg_aw_count, dut.dbg_w_count, dut.dbg_b_count);
            if ((deadlock_limit != 0) && (idle_cycles == deadlock_limit)) begin
                $display("DEADLOCK cycle=%0d: renderer busy with no memory or command handshake for %0d cycles",
                         cycle_count, idle_cycles);
                dump_memory_state;
                $fatal(1, "memory deadlock");
            end
        end
    end
`endif

    // Framebuffer command evidence: correlate every OP_FRAMEBUFFER the parser
    // accepts with the per-port address requests the handlers raise, so a
    // crossbar stall can be attributed to a concrete command word.
    reg [31:0] last_fb_word;
    reg [2:0] fb_avalid_prev;
`ifndef RASTERIX_VARIANT_EF
    always @(posedge clk) begin
        if (resetn &&
            (dut.renderer.rixif.graphicCore.commandParser.state == 1) &&
            dut.renderer.rixif.graphicCore.commandParser.s_cmd_axis_tvalid &&
            dut.renderer.rixif.graphicCore.commandParser.s_cmd_axis_tready &&
            (dut.renderer.rixif.graphicCore.commandParser.s_cmd_axis_tdata[31:28] == 4'h2)) begin
            last_fb_word <= dut.renderer.rixif.graphicCore.commandParser.s_cmd_axis_tdata;
            $display("FB_OP cycle=%0d word=%08x commit=%b memset=%b swap=%b read=%b sel[cds]=%b%b%b size=%0d",
                     cycle_count,
                     dut.renderer.rixif.graphicCore.commandParser.s_cmd_axis_tdata,
                     dut.renderer.rixif.graphicCore.commandParser.s_cmd_axis_tdata[0],
                     dut.renderer.rixif.graphicCore.commandParser.s_cmd_axis_tdata[1],
                     dut.renderer.rixif.graphicCore.commandParser.s_cmd_axis_tdata[2],
                     dut.renderer.rixif.graphicCore.commandParser.s_cmd_axis_tdata[3],
                     dut.renderer.rixif.graphicCore.commandParser.s_cmd_axis_tdata[6],
                     dut.renderer.rixif.graphicCore.commandParser.s_cmd_axis_tdata[5],
                     dut.renderer.rixif.graphicCore.commandParser.s_cmd_axis_tdata[4],
                     dut.renderer.rixif.graphicCore.commandParser.s_cmd_axis_tdata[27:8]);
        end
        if (resetn && (dut.renderer.framebuffer_avalid != fb_avalid_prev)) begin
            $display("FB_AVALID cycle=%0d ports=%b last_fb_word=%08x color[state=%0d avalid=%b beats=%0d] depth[state=%0d avalid=%b beats=%0d] xbar[active=%b port=%0d count=%0d]",
                     cycle_count, dut.renderer.framebuffer_avalid, last_fb_word,
                     dut.renderer.rixif.colorBuffer.commandHandler.cmdState,
                     dut.renderer.rixif.colorBuffer.commandHandler.m_avalid,
                     dut.renderer.rixif.colorBuffer.commandHandler.m_abeats,
                     dut.renderer.rixif.genDepthBuffer.depthBuffer.commandHandler.cmdState,
                     dut.renderer.rixif.genDepthBuffer.depthBuffer.commandHandler.m_avalid,
                     dut.renderer.rixif.genDepthBuffer.depthBuffer.commandHandler.m_abeats,
                     dut.renderer.axisToAxiCrossbar.transferActive,
                     dut.renderer.axisToAxiCrossbar.portSelect,
                     dut.renderer.axisToAxiCrossbar.axisToAxiAdapter.counter);
        end
        fb_avalid_prev <= dut.renderer.framebuffer_avalid;
    end
`endif

    task axi_write;
        input [15:0] addr;
        input [31:0] data;
        begin
            @(posedge clk);
            s_axi_awaddr <= addr; s_axi_awvalid <= 1;
            s_axi_wdata <= data; s_axi_wstrb <= 4'hf; s_axi_wvalid <= 1;
            while (!(s_axi_awvalid && s_axi_awready)) @(posedge clk);
            s_axi_awvalid <= 0;
            while (!(s_axi_wvalid && s_axi_wready)) @(posedge clk);
            s_axi_wvalid <= 0;
            while (!s_axi_bvalid) @(posedge clk);
            @(posedge clk);
        end
    endtask
    task axi_read;
        input [15:0] addr;
        output [31:0] data;
        begin
            @(posedge clk);
            s_axi_araddr <= addr; s_axi_arvalid <= 1;
            while (!(s_axi_arvalid && s_axi_arready)) @(posedge clk);
            s_axi_arvalid <= 0;
            while (!s_axi_rvalid) @(posedge clk);
            data = s_axi_rdata;
            @(posedge clk);
        end
    endtask
    // Models the AXI FIFO MM-S TX stream after a complete TDFD/TLF packet.
    // The producer advances strictly on completed AXI-Stream handshakes: the
    // wait condition samples tvalid/tready of the cycle each posedge closes,
    // so a word can never be replaced before its acceptance edge nor held
    // across one (which the DUT reads as a dropped or duplicated command).
    // DMA_BURST preserves these packet boundaries while changing only producer
    // writes, so both transports share this sink-side model.
    task fifo_submit;
        input integer start_word;
        input integer word_count;
        integer n;
        begin
            @(posedge clk);
            for (n = 0; n < word_count; n = n + 1) begin
                s_cmd_axis_tdata <= command_words[start_word + n];
                s_cmd_axis_tlast <= (n == word_count - 1);
                s_cmd_axis_tvalid <= 1;
                do @(posedge clk);
                while (!(s_cmd_axis_tvalid && s_cmd_axis_tready));
            end
            s_cmd_axis_tvalid <= 0;
            s_cmd_axis_tlast <= 0;
        end
    endtask
    task dump_ppm;
        input [8*512-1:0] name;
        integer fd, x, y;
        reg [15:0] pixel;
        reg [7:0] red, green, blue;
        begin
            fd = $fopen(name, "wb");
            if (fd == 0) $fatal(1, "cannot open ppm");
            $fwrite(fd, "P6\n640 480\n255\n");
            for (y = 0; y < HEIGHT; y = y + 1) begin
                for (x = 0; x < WIDTH; x = x + 1) begin
                    pixel = {byte_at(framebuffer_addr + ((HEIGHT - 1 - y) * WIDTH + x) * 2 + 1),
                             byte_at(framebuffer_addr + ((HEIGHT - 1 - y) * WIDTH + x) * 2)};
                    red = {pixel[15:11], 3'b000};
                    green = {pixel[10:5], 2'b00};
                    blue = {pixel[4:0], 3'b000};
                    $fwrite(fd, "%c%c%c", red, green, blue);
                end
            end
            $fclose(fd);
        end
    endtask
    // Sparse DDR input: $readmemh-style @<byte-address> markers followed by
    // one byte per line. Unspecified bytes remain zero.
    task load_ddr;
        input [8*512-1:0] name;
        integer fd;
        integer scan_result;
        reg [8*1024-1:0] line;
        reg [31:0] load_addr;
        reg [7:0] load_value;
        begin
            fd = $fopen(name, "r");
            if (fd == 0) $fatal(1, "cannot open DDR preload %0s", name);
            load_addr = 0;
            while (!$feof(fd)) begin
                line = 0;
                scan_result = $fgets(line, fd);
                if ($sscanf(line, "@%h", load_addr) == 1) begin
                end else if ($sscanf(line, "%h", load_value) == 1) begin
                    write_byte(load_addr, load_value);
                    load_addr = load_addr + 1;
                end
            end
            $fclose(fd);
        end
    endtask
    task print_timeout;
        begin
            $display("TIMEOUT cycle=%0d status=%08x stream=%b/%b renderer busy=%b",
                     cycle_count, s_axi_rdata, s_cmd_axis_tvalid, s_cmd_axis_tready,
                     dut.renderer.perfBusy);
`ifdef RASTERIX_VARIANT_EF
            $display("DSE state=%0d EF parser state=%0d fbctl=%0d color apply/applied=%b/%b depth=%b/%b", 
                     dut.renderer.dma.state, dut.renderer.rixef.graphicCore.commandParser.state,
                     dut.renderer.rixef.graphicCore.commandParser.fbControlState,
                     dut.renderer.rixef.graphicCore.commandParser.colorBufferApply,
                     dut.renderer.rixef.graphicCore.commandParser.colorBufferApplied,
                     dut.renderer.rixef.graphicCore.commandParser.depthBufferApply,
                     dut.renderer.rixef.graphicCore.commandParser.depthBufferApplied);
            $display("EF renderer busy=%b triangle=%b stall=%b",
                     dut.renderer.perfBusy, dut.renderer.perfTriangleRendering,
                     dut.renderer.perfRasterizerStall);
            dump_memory_state;
`else
            $display("DSE state=%0d cmd parser state=%0d fbctl=%0d color apply/applied=%b/%b depth=%b/%b", 
                     dut.renderer.dma.state, dut.renderer.rixif.graphicCore.commandParser.state,
                     dut.renderer.rixif.graphicCore.commandParser.fbControlState,
                     dut.renderer.rixif.graphicCore.commandParser.colorBufferApply,
                     dut.renderer.rixif.graphicCore.commandParser.colorBufferApplied,
                     dut.renderer.rixif.graphicCore.commandParser.depthBufferApply,
                     dut.renderer.rixif.graphicCore.commandParser.depthBufferApplied);
            $display("color handler state=%0d applied=%b avalid=%b depth state=%0d applied=%b avalid=%b",
                     dut.renderer.rixif.colorBuffer.commandHandler.cmdState,
                     dut.renderer.rixif.colorBuffer.commandHandler.applied,
                     dut.renderer.rixif.colorBuffer.commandHandler.m_avalid,
                     dut.renderer.rixif.genDepthBuffer.depthBuffer.commandHandler.cmdState,
                     dut.renderer.rixif.genDepthBuffer.depthBuffer.commandHandler.applied,
                     dut.renderer.rixif.genDepthBuffer.depthBuffer.commandHandler.m_avalid);
            $display("xbar active=%b port=%0d adapter streaming=%b address_done=%b count=%0d renderer busy=%b",
                     dut.renderer.axisToAxiCrossbar.transferActive, dut.renderer.axisToAxiCrossbar.portSelect,
                     dut.renderer.axisToAxiCrossbar.axisToAxiAdapter.streamingDone,
                     dut.renderer.axisToAxiCrossbar.axisToAxiAdapter.addressGenerationDone,
                     dut.renderer.axisToAxiCrossbar.axisToAxiAdapter.counter, dut.renderer.perfBusy);
`endif
        end
    endtask

    reg [8*512-1:0] command_file;
    reg [8*512-1:0] commands_file;
    reg [8*512-1:0] ddr_file;
    reg [8*512-1:0] patch_file;
    reg [8*512-1:0] texture_file;
    reg [8*512-1:0] ppm_file;
    reg [8*32-1:0] scene;
    reg [31:0] framebuffer_addr;
    reg [31:0] status;
    reg [31:0] completed_before;
    integer cmd_count;
    integer submission_offset;
    integer submission_words;
    integer submission_index;
    integer command_complete;
    integer multi_command;
    integer capture_mode;
    integer size_probe;
    integer patch_after;
    integer timeout;
    integer poll_limit;
    reg [15:0] sample0, sample_mid, sample_out;

    initial begin
        clk = 0; resetn = 0; cycle_count = 0; latency = 2;
        s_axi_awaddr = 0; s_axi_awprot = 0; s_axi_awvalid = 0; s_axi_wdata = 0; s_axi_wstrb = 0; s_axi_wvalid = 0; s_axi_bready = 1;
        s_axi_araddr = 0; s_axi_arprot = 0; s_axi_arvalid = 0; s_axi_rready = 1; s_cmd_axis_tvalid = 0; s_cmd_axis_tlast = 0; s_cmd_axis_tdata = 0;
        m_axi_awready = 0; m_axi_wready = 0; m_axi_bid = 0; m_axi_bresp = 0; m_axi_bvalid = 0;
        m_axi_arready = 0; m_axi_rid = 0; m_axi_rdata = 0; m_axi_rresp = 0; m_axi_rlast = 0; m_axi_rvalid = 0;
        m_cmd_axi_arready = 0; m_cmd_axi_rid = 0; m_cmd_axi_rdata = 0; m_cmd_axi_rresp = 0; m_cmd_axi_rlast = 0; m_cmd_axi_rvalid = 0;
        cmd_read_active = 0; render_read_active = 0; write_active = 0; write_error = 0; cmd_read_left = 0; render_read_left = 0; write_left = 0;
        render_read_id = 0; render_read_size = 0;
        shared_channel = $test$plusargs("SHARED");
        rand_seed = 0;
        if ($value$plusargs("RAND_SEED=%d", rand_seed)) ;
        rand_state = rand_seed;
        rand_latency = 16;
        if ($value$plusargs("RAND_LATENCY=%d", rand_latency)) ;
        shared_q_count = 0; shared_served = 0; shared_reordered = 0;
        shared_active = 0; shared_addr = 0;
        shared_left = 0; shared_delay = 0; shared_id = 0; shared_size = 0;
        for (i = 0; i < 32; i = i + 1) begin
            shared_q_addr[i] = 0;
            shared_q_len[i] = 0;
            shared_q_id[i] = 0;
            shared_q_size[i] = 0;
        end
        multi_command = $value$plusargs("CMDS=%s", commands_file);
        if (multi_command == 0) begin
            if (!$value$plusargs("CMD=%s", command_file)) $fatal(1, "CMD=<scene>.hex or CMDS=<capture>.hex is required");
        end else
            command_file = commands_file;
        capture_mode = $value$plusargs("DDR=%s", ddr_file);
        size_probe = $test$plusargs("SIZE_PROBE");
        if ($value$plusargs("PATCH=%s", patch_file)) begin
            if (!$value$plusargs("PATCH_AFTER=%d", patch_after) || patch_after < 0)
                $fatal(1, "PATCH requires non-negative PATCH_AFTER");
        end
        framebuffer_addr = capture_mode ? MINIGL_COLOR_ADDR : COLOR_ADDR;
        if (!$value$plusargs("TEXTURE=%s", texture_file)) texture_file = "";
        if (!$value$plusargs("PPM=%s", ppm_file)) ppm_file = "frame.ppm";
        if (!$value$plusargs("SCENE=%s", scene)) scene = "s1";
        if ($value$plusargs("LATENCY=%d", latency)) ;
        trace_pipeline = 0;
        if ($value$plusargs("TRACE_PIPE=%d", trace_pipeline)) ;
`ifdef RASTERIX_VARIANT_EF
        trace_axi = 0;
        if ($value$plusargs("TRACE_AXI=%d", trace_axi)) ;
        deadlock_limit = 200000;
        if ($value$plusargs("DEADLOCK_LIMIT=%d", deadlock_limit)) ;
`endif
        poll_limit = 2000000;
        if ($value$plusargs("POLL_LIMIT=%d", poll_limit)) ;
        for (i = 0; i < MEM_BYTES; i = i + 1) memory[i] = 0;
        for (i = 0; i < MINIGL_COLOR_MEMORY_BYTES; i = i + 1) color_memory[i] = 0;
        for (i = 0; i < MINIGL_DEPTH_MEMORY_BYTES; i = i + 1) depth_memory[i] = 0;
        for (i = 0; i < TEXTURE_POOL_BYTES; i = i + 1) texture_memory[i] = 0;
        if (capture_mode != 0) load_ddr(ddr_file);
        if (texture_file != "") begin
            $readmemh(texture_file, texture_bytes);
            for (i = 0; i < TEXTURE_BYTES; i = i + 1) write_byte(32'h0034_0000 + i, texture_bytes[i]);
        end
        $readmemh(command_file, command_words);
        if (multi_command != 0) begin
            cmd_count = 0;
            while ((cmd_count < 262144) && (command_words[cmd_count][31:28] === 4'h9)) begin
                if (command_words[cmd_count][1:0] != 0)
                    $fatal(1, "CMDS buffer %0d has unaligned byte count", cmd_count);
                submission_words = (command_words[cmd_count] & 28'h0fff_ffff) / 4 + 2;
                if ((submission_words < 2) || (cmd_count + submission_words > 262144))
                    $fatal(1, "CMDS buffer at word %0d is invalid", cmd_count);
                cmd_count = cmd_count + submission_words;
            end
            if (cmd_count == 0) $fatal(1, "CMDS contains no DSE buffers");
        end else
            cmd_count = (command_words[0] & 28'h0fff_ffff) / 4 + 2;
        repeat (8) @(posedge clk);
        resetn = 1;
        axi_read(16'h0000, status);
        if (status != 32'h52495831) $fatal(1, "bad RIX identity %08x", status);
        axi_write(16'h0008, 1);
        submission_offset = 0;
        submission_index = 0;
        while (submission_offset < cmd_count) begin
            submission_words = (command_words[submission_offset] & 28'h0fff_ffff) / 4 + 2;
            fifo_submit(submission_offset, submission_words);
            timeout = 0;
            command_complete = 0;
            while ((timeout < poll_limit) && (command_complete == 0)) begin
                axi_read(16'h000c, status);
                if (status[1]) begin
`ifdef RASTERIX_VARIANT_EF
                    /* EF STATUS.IDLE only reports the command FIFO and the
                     * memory-transaction counters drained; the StreamFramebuffer
                     * memset engines start a few cycles after FIFO acceptance
                     * and keep running past it.  The parser holds
                     * FB_CONTROL_WAITFOREND until every selected buffer
                     * reported applied, so settle first, then gate on the
                     * parser being back between commands with idle still set. */
                    command_complete = 0;
                    while ((timeout < poll_limit) && (command_complete == 0)) begin
                        repeat (16) @(posedge clk);
                        timeout = timeout + 16;
                        axi_read(16'h000c, status);
                        if (status[1] &&
                            (dut.renderer.rixef.graphicCore.commandParser.fbControlState == 0))
                            command_complete = 1;
                    end
`else
                    /* FIFO acceptance is not the IF framebuffer drain point:
                     * wait for the final commit before dumping a replay. */
                    while ((timeout < poll_limit) &&
                           (((last_fb_word & 32'h1) == 0) ||
                            dut.renderer.framebuffer_avalid != 0 ||
                            dut.renderer.rixif.colorBuffer.commandHandler.cmdState != 0 ||
                            dut.renderer.rixif.genDepthBuffer.depthBuffer.commandHandler.cmdState != 0 ||
                            dut.renderer.axisToAxiCrossbar.transferActive != 0)) begin
                        @(posedge clk);
                        timeout = timeout + 1;
                    end
`endif
                    if (timeout < poll_limit) begin
                        $display("RESULT scene=%0s submission=%0d cycles=%0d command_words=%0d transport=%0s",
                                 scene, submission_index, cycle_count, submission_words,
                                 $test$plusargs("DMA_BURST") ? "dma-fixed" : "mmio");
                        if (shared_channel)
                            $display("SHARED_READS served=%0d reordered=%0d rand_seed=%0d",
                                     shared_served, shared_reordered, rand_seed);
                        command_complete = 1;
                    end
                end
                timeout = timeout + 1;
            end
            if (command_complete == 0) begin
                axi_read(16'h000c, status);
                print_timeout;
                $fatal(1, "completion timeout");
            end
            if (submission_index == patch_after) load_ddr(patch_file);
            submission_offset = submission_offset + submission_words;
            submission_index = submission_index + 1;
        end
        dump_ppm(ppm_file);
        for (i = 0; i < COLOR_BYTES; i = i + 1)
            if (^byte_at(framebuffer_addr + i) === 1'bx)
                $fatal(1, "framebuffer contains unknown byte at %0d", i);
        if (capture_mode != 0 || size_probe != 0) begin
            $display("PASS scene=%0s submissions=%0d", scene, submission_index);
        end else begin
            begin : CountNonClear
                integer non_clear_count, first_nc_addr;
                reg [15:0] first_nc_val;
                non_clear_count = 0; first_nc_addr = -1; first_nc_val = 0;
                for (i = 0; i < COLOR_BYTES; i = i + 2) begin
                    if ({byte_at(framebuffer_addr + i + 1), byte_at(framebuffer_addr + i)} != 16'hf800) begin
                        if (non_clear_count == 0) begin
                            first_nc_addr = i;
                            first_nc_val = {byte_at(framebuffer_addr + i + 1), byte_at(framebuffer_addr + i)};
                        end
                        non_clear_count = non_clear_count + 1;
                    end
                end
                $display("NON_CLEAR_PIXELS = %0d first_addr=%x first_val=%04x", non_clear_count, first_nc_addr, first_nc_val);
            end
            sample0 = {byte_at(framebuffer_addr + 1), byte_at(framebuffer_addr)};
            sample_out = {byte_at(framebuffer_addr + ((HEIGHT - 1 - 10) * WIDTH + 10) * 2 + 1),
                          byte_at(framebuffer_addr + ((HEIGHT - 1 - 10) * WIDTH + 10) * 2)};
            if (scene == "s4" || scene == "ef-s4")
                sample_mid = {byte_at(framebuffer_addr + ((HEIGHT - 1 - 330) * WIDTH + 200) * 2 + 1),
                              byte_at(framebuffer_addr + ((HEIGHT - 1 - 330) * WIDTH + 200) * 2)};
            else
                sample_mid = {byte_at(framebuffer_addr + ((HEIGHT - 1 - 240) * WIDTH + 320) * 2 + 1),
                              byte_at(framebuffer_addr + ((HEIGHT - 1 - 240) * WIDTH + 320) * 2)};
            $display("SAMPLES scene=%0s clear=%04x interior=%04x exterior=%04x", scene, sample0, sample_mid, sample_out);
            if (sample0 !== 16'hf800 || sample_out !== 16'hf800) $fatal(1, "clear/exterior expected red got %04x %04x", sample0, sample_out);
            if (scene != "s1" && scene != "ef-s1" &&
                (sample_mid === 16'hf800 || (^sample_mid === 1'bx)))
                $fatal(1, "triangle interior is clear or unknown: %04x", sample_mid);
            $display("PASS scene=%0s samples=%04x/%04x/%04x", scene, sample0, sample_mid, sample_out);
        end
        $finish;
    end
endmodule
