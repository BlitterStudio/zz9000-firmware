`timescale 1ns/1ps
// Real ZZ9000 RasterIX integration testbench.  The command buffer is an ARM
// DDR image: FrameStreamingCore header/address followed by parser words.
module tb_zz9k_rasterix;
    localparam integer MEM_BYTES = 4 * 1024 * 1024;
    localparam [31:0] CMD_ADDR = 32'h0010_0000;
    localparam [31:0] COLOR_ADDR = 32'h0020_0000;
    localparam integer WIDTH = 640;
    localparam integer HEIGHT = 480;
    localparam integer COLOR_BYTES = WIDTH * HEIGHT * 2;

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

    wire [3:0] m_axi_awid;
    wire [31:0] m_axi_awaddr;
    wire [7:0] m_axi_awlen;
    wire [2:0] m_axi_awsize;
    wire [1:0] m_axi_awburst;
    wire m_axi_awlock;
    wire [3:0] m_axi_awcache;
    wire [2:0] m_axi_awprot;
    wire m_axi_awvalid;
    reg m_axi_awready;
    wire [31:0] m_axi_wdata;
    wire [3:0] m_axi_wstrb;
    wire m_axi_wlast;
    wire m_axi_wvalid;
    reg m_axi_wready;
    reg [3:0] m_axi_bid;
    reg [1:0] m_axi_bresp;
    reg m_axi_bvalid;
    wire m_axi_bready;
    wire [3:0] m_axi_arid;
    wire [31:0] m_axi_araddr;
    wire [7:0] m_axi_arlen;
    wire [2:0] m_axi_arsize;
    wire [1:0] m_axi_arburst;
    wire m_axi_arlock;
    wire [3:0] m_axi_arcache;
    wire [2:0] m_axi_arprot;
    wire m_axi_arvalid;
    reg m_axi_arready;
    reg [3:0] m_axi_rid;
    reg [31:0] m_axi_rdata;
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
        .m_axi_rvalid(m_axi_rvalid), .m_axi_rready(m_axi_rready),
        .m_cmd_axi_arid(m_cmd_axi_arid), .m_cmd_axi_araddr(m_cmd_axi_araddr),
        .m_cmd_axi_arlen(m_cmd_axi_arlen), .m_cmd_axi_arsize(m_cmd_axi_arsize),
        .m_cmd_axi_arburst(m_cmd_axi_arburst), .m_cmd_axi_arlock(m_cmd_axi_arlock),
        .m_cmd_axi_arcache(m_cmd_axi_arcache), .m_cmd_axi_arprot(m_cmd_axi_arprot),
        .m_cmd_axi_arvalid(m_cmd_axi_arvalid), .m_cmd_axi_arready(m_cmd_axi_arready),
        .m_cmd_axi_rid(m_cmd_axi_rid), .m_cmd_axi_rdata(m_cmd_axi_rdata), .m_cmd_axi_rresp(m_cmd_axi_rresp),
        .m_cmd_axi_rlast(m_cmd_axi_rlast), .m_cmd_axi_rvalid(m_cmd_axi_rvalid), .m_cmd_axi_rready(m_cmd_axi_rready)
    );

    reg [7:0] memory [0:MEM_BYTES-1];
    reg [31:0] command_words [0:262143];
    reg [7:0] texture_bytes [0:8191];
    integer i;
    integer latency;
    integer cycle_count;
    reg [31:0] cmd_read_addr;
    integer cmd_read_left;
    integer cmd_read_delay;
    reg cmd_read_active;
    reg [31:0] render_read_addr;
    integer render_read_left;
    integer render_read_delay;
    reg render_read_active;
    reg [31:0] write_addr;
    integer write_left;
    reg write_active;
    reg [3:0] write_id;

    function [7:0] byte_at;
        input [31:0] address;
        begin
            if (address < MEM_BYTES) byte_at = memory[address]; else byte_at = 8'h00;
        end
    endfunction
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
    always @* begin
        m_cmd_axi_arready = !cmd_read_active && !m_cmd_axi_rvalid && ((cycle_count % 7) != 0);
        m_axi_arready = !render_read_active && !m_axi_rvalid && ((cycle_count % 5) != 0);
        m_axi_awready = !write_active && !m_axi_bvalid && ((cycle_count % 6) != 0);
        m_axi_wready = write_active && ((cycle_count % 4) != 0);
    end

    always @(posedge clk) begin
        cycle_count <= cycle_count + 1;

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
            render_read_addr <= m_axi_araddr;
            render_read_left <= m_axi_arlen + 1;
            render_read_delay <= latency;
            render_read_active <= 1;
        end
        if (render_read_active && !m_axi_rvalid) begin
            if (render_read_delay != 0) render_read_delay <= render_read_delay - 1;
            else begin
                m_axi_rvalid <= 1;
                m_axi_rdata <= word_at(render_read_addr);
                m_axi_rid <= m_axi_arid;
                m_axi_rresp <= 0;
                m_axi_rlast <= (render_read_left == 1);
            end
        end
        if (m_axi_rvalid && m_axi_rready) begin
            m_axi_rvalid <= 0;
            if (render_read_left == 1) begin
                render_read_active <= 0;
                m_axi_rlast <= 0;
            end else begin
                render_read_addr <= render_read_addr + (1 << m_axi_arsize);
                render_read_left <= render_read_left - 1;
                render_read_delay <= latency;
            end
        end

        if (m_axi_awvalid && m_axi_awready) begin
            write_active <= 1;
            write_addr <= m_axi_awaddr;
            write_left <= m_axi_awlen + 1;
            write_id <= m_axi_awid;
        end
        if (m_axi_wvalid && m_axi_wready) begin
            if (m_axi_wstrb[0]) memory[write_addr] <= m_axi_wdata[7:0];
            if (m_axi_wstrb[1]) memory[write_addr + 1] <= m_axi_wdata[15:8];
            if (m_axi_wstrb[2]) memory[write_addr + 2] <= m_axi_wdata[23:16];
            if (m_axi_wstrb[3]) memory[write_addr + 3] <= m_axi_wdata[31:24];
            write_addr <= write_addr + 4;
            write_left <= write_left - 1;
            if (m_axi_wlast) begin
                if (write_left != 1) $fatal(1, "early renderer WLAST left=%0d addr=%08x awlen=%0d", write_left, write_addr, m_axi_awlen);
                write_active <= 0;
                m_axi_bvalid <= 1;
                m_axi_bid <= write_id;
                m_axi_bresp <= 0;
            end
        end
        if (m_axi_bvalid && m_axi_bready) m_axi_bvalid <= 0;
    end

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
                    pixel = {memory[COLOR_ADDR + ((HEIGHT - 1 - y) * WIDTH + x) * 2 + 1],
                             memory[COLOR_ADDR + ((HEIGHT - 1 - y) * WIDTH + x) * 2]};
                    red = {pixel[15:11], 3'b000};
                    green = {pixel[10:5], 2'b00};
                    blue = {pixel[4:0], 3'b000};
                    $fwrite(fd, "%c%c%c", red, green, blue);
                end
            end
            $fclose(fd);
        end
    endtask
    task print_timeout;
        begin
            $display("TIMEOUT cycle=%0d status=%08x fetch.state=%0d words_left=%0d fifo=%0d", cycle_count,
                     s_axi_rdata, dut.command_fetcher.state, dut.command_fetcher.words_left, dut.command_count);
            $display("cmd-ddr active=%b left=%0d rvalid=%b rready=%b rlast=%b arvalid=%b arready=%b",
                     cmd_read_active, cmd_read_left, m_cmd_axi_rvalid, m_cmd_axi_rready,
                     m_cmd_axi_rlast, m_cmd_axi_arvalid, m_cmd_axi_arready);
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
        end
    endtask

    reg [8*512-1:0] command_file;
    reg [8*512-1:0] texture_file;
    reg [8*512-1:0] ppm_file;
    reg [8*32-1:0] scene;
    reg [31:0] status;
    reg [31:0] completed_before;
    integer cmd_count;
    integer timeout;
    integer poll_limit;
    reg [15:0] sample0, sample_mid, sample_out;

    initial begin
        clk = 0; resetn = 0; cycle_count = 0; latency = 2;
        s_axi_awaddr = 0; s_axi_awprot = 0; s_axi_awvalid = 0; s_axi_wdata = 0; s_axi_wstrb = 0; s_axi_wvalid = 0; s_axi_bready = 1;
        s_axi_araddr = 0; s_axi_arprot = 0; s_axi_arvalid = 0; s_axi_rready = 1;
        m_axi_awready = 0; m_axi_wready = 0; m_axi_bid = 0; m_axi_bresp = 0; m_axi_bvalid = 0;
        m_axi_arready = 0; m_axi_rid = 0; m_axi_rdata = 0; m_axi_rresp = 0; m_axi_rlast = 0; m_axi_rvalid = 0;
        m_cmd_axi_arready = 0; m_cmd_axi_rid = 0; m_cmd_axi_rdata = 0; m_cmd_axi_rresp = 0; m_cmd_axi_rlast = 0; m_cmd_axi_rvalid = 0;
        cmd_read_active = 0; render_read_active = 0; write_active = 0; cmd_read_left = 0; render_read_left = 0; write_left = 0;
        if (!$value$plusargs("CMD=%s", command_file)) $fatal(1, "CMD=<scene>.hex is required");
        if (!$value$plusargs("TEXTURE=%s", texture_file)) texture_file = "";
        if (!$value$plusargs("PPM=%s", ppm_file)) ppm_file = "frame.ppm";
        if (!$value$plusargs("SCENE=%s", scene)) scene = "s1";
        if ($value$plusargs("LATENCY=%d", latency)) ;
        poll_limit = 2000000;
        if ($value$plusargs("POLL_LIMIT=%d", poll_limit)) ;
        for (i = 0; i < MEM_BYTES; i = i + 1) memory[i] = 0;
        if (texture_file != "") begin
            $readmemh(texture_file, texture_bytes);
            for (i = 0; i < 8192; i = i + 1) memory[32'h0034_0000 + i] = texture_bytes[i];
        end
        $readmemh(command_file, command_words);
        cmd_count = (command_words[0] & 28'h0fff_ffff) / 4 + 2;
        for (i = 0; i < cmd_count; i = i + 1) begin
            memory[CMD_ADDR + i * 4] = command_words[i][7:0];
            memory[CMD_ADDR + i * 4 + 1] = command_words[i][15:8];
            memory[CMD_ADDR + i * 4 + 2] = command_words[i][23:16];
            memory[CMD_ADDR + i * 4 + 3] = command_words[i][31:24];
        end
        repeat (8) @(posedge clk);
        resetn = 1;
        axi_read(16'h0000, status);
        if (status != 32'h52495831) $fatal(1, "bad RIX identity %08x", status);
        axi_write(16'h0008, 1);
        axi_read(16'h002c, completed_before);
        axi_write(16'h001c, CMD_ADDR);
        axi_write(16'h0020, cmd_count);
        axi_write(16'h0024, 1);
        timeout = 0;
        while (timeout < poll_limit) begin
            axi_read(16'h002c, status);
            if (status == completed_before + 1) begin
                $display("RESULT scene=%0s cycles=%0d command_words=%0d", scene, cycle_count, cmd_count);
                dump_ppm(ppm_file);
                sample0 = {memory[COLOR_ADDR+1], memory[COLOR_ADDR]};
                sample_out = {memory[COLOR_ADDR + ((HEIGHT - 1 - 10) * WIDTH + 10) * 2 + 1], memory[COLOR_ADDR + ((HEIGHT - 1 - 10) * WIDTH + 10) * 2]};
                sample_mid = {memory[COLOR_ADDR + ((HEIGHT - 1 - 240) * WIDTH + 320) * 2 + 1], memory[COLOR_ADDR + ((HEIGHT - 1 - 240) * WIDTH + 320) * 2]};
                if (sample0 !== 16'hf800 || sample_out !== 16'hf800) $fatal(1, "clear/exterior expected red got %04x %04x", sample0, sample_out);
                if (scene != "s1" && sample_mid === 16'hf800) $fatal(1, "triangle interior remained clear");
                $display("PASS scene=%0s samples=%04x/%04x/%04x", scene, sample0, sample_mid, sample_out);
                $finish;
            end
            timeout = timeout + 1;
        end
        axi_read(16'h000c, status);
        print_timeout;
        $fatal(1, "completion timeout");
    end
endmodule
