`timescale 1ns / 1ps

/* All three native input topologies must store the RGB values presented by
 * their actual pin widths. Tests the sampled line-buffer words, not defines. */
module videocap_variant_tb;
    reg cap_clk = 0, axi_clk = 0, cap_reset = 1;
    reg hsync = 1, vsync = 1;
    reg [7:0] r = 8'ha3, g = 8'h5c, b = 8'he7;
    reg [2:0] read_bank = 0;
    wire [2:0] write_bank, line_toggle;
    wire [31:0] pixel [0:2];
    wire [9:0] token_y [0:2];
    wire [10:0] visible_y [0:2];
    integer tokens [0:2];
    reg [2:0] previous_toggle = 0;
    integer i;
    integer monitor_mode;
    genvar variant_mode;

    always #17.6 cap_clk = ~cap_clk;
    always #5 axi_clk = ~axi_clk;

    generate for (variant_mode = 0; variant_mode < 3; variant_mode = variant_mode + 1) begin : variants
        localparam integer FULLRATE = (variant_mode == 1) ? 0 : 1;
        localparam integer CSYNC_VSYNC = (variant_mode == 1) ? 1 : 0;
        videocap_sampler #(
            .BUF_DEPTH(2048), .RGB_MODE(variant_mode),
            .CSYNC_VSYNC(CSYNC_VSYNC), .FULLRATE(FULLRATE)
        ) dut (
            .cap_clk(cap_clk), .cap_reset(cap_reset),
            .vcap_vsync(vsync), .vcap_hsync(hsync),
            .vcap_r(r), .vcap_g(g), .vcap_b(b), .grid_ref(1'b0),
            .ctl_send(1'b0),
            .ctl_payload({12'd26, 12'd188, 1'b0, 2'd0}),
            .ctl_read_full_width(1'b0),
            .cap_line_toggle(line_toggle[variant_mode]),
            .cap_y(visible_y[variant_mode]),
            .cap_write_bank(write_bank[variant_mode]),
            .cap_token_y(token_y[variant_mode]),
            .probe_arm_toggle(1'b0), .probe_precrop_raddr(6'd0),
            .axi_clk(axi_clk), .axi_resetn(1'b1), .cal_arm(1'b0),
            .cal_address(10'd0), .cal_metadata_address(4'd0),
            .buf_rbank(read_bank[variant_mode]), .buf_raddr(12'd32),
            .buf_rdata(pixel[variant_mode])
        );
    end endgenerate

    always @(posedge cap_clk) begin
        for (monitor_mode = 0; monitor_mode < 3; monitor_mode = monitor_mode + 1) begin
            if (line_toggle[monitor_mode] != previous_toggle[monitor_mode])
                tokens[monitor_mode] <= tokens[monitor_mode] + 1;
            previous_toggle[monitor_mode] <= line_toggle[monitor_mode];
        end
    end

    task drive_line;
        input integer sync_clocks;
        begin
            @(negedge cap_clk); hsync = 0;
            repeat (sync_clocks) @(negedge cap_clk);
            hsync = 1;
            repeat (1816 - sync_clocks) @(negedge cap_clk);
        end
    endtask

    initial begin
        for (i = 0; i < 3; i = i + 1) tokens[i] = 0;
        repeat (10) @(negedge cap_clk);
        cap_reset = 0;
        // Drain two reset-recovery fields before checking visible lines.
        for (i = 0; i < 2; i = i + 1) begin
            vsync = 0;
            drive_line(150);
            vsync = 1;
            repeat (4) drive_line(67);
        end
        vsync = 0;
        drive_line(150);
        vsync = 1;
        for (i = 0; i < 34; i = i + 1) drive_line(67);

        for (i = 0; i < 3; i = i + 1) read_bank[i] = write_bank[i];
        repeat (2) @(posedge axi_clk);
        #0.001;
        if (pixel[0][23:0] !== 24'ha35ce7 ||
            pixel[1][23:0] !== 24'h33cc77 ||
            pixel[2][23:0] !== 24'haa55ee) begin
            $display("RESULT FAIL RGB topology: slot=%h Denise=%h Z2=%h",
                     pixel[0], pixel[1], pixel[2]);
            $finish;
        end
        if (tokens[0] == 0 || tokens[1] == 0 || tokens[2] == 0 ||
            token_y[0] > 10'd10 || token_y[1] > 10'd10 ||
            token_y[2] > 10'd10) begin
            $display("RESULT FAIL visible line tokens: slot=%0d/%0d Denise=%0d/%0d Z2=%0d/%0d",
                     tokens[0], visible_y[0], tokens[1], visible_y[1],
                     tokens[2], visible_y[2]);
            $finish;
        end
        $display("RESULT PASS RGB topology: slot=%h Denise=%h Z2=%h",
                 pixel[0][23:0], pixel[1][23:0], pixel[2][23:0]);
        $finish;
    end

    initial begin
        // Keep the bound below 2^32 ps for older Verilator event queues.
        #4000000;
        $display("RESULT FAIL RGB topology: timeout");
        $finish;
    end
endmodule
