`timescale 1ns/1ps
/* Coherent crop/line-count publication under asynchronous polling and updates
 * during a transfer. The production register mux also exercises the Z2 word
 * half. Test CDC models do not qualify metastability or routed timing.
 * SPDX-License-Identifier: GPL-3.0-or-later */
module videocap_live_publish_tb;
    reg cap_clk = 0, axi_clk = 0;
    reg axi_running = 1;
    always #7 cap_clk = ~cap_clk;
    always #5 if (axi_running) axi_clk = ~axi_clk;

    reg [11:0] crop_h = 188, crop_v = 26;
    reg [9:0] line_count = 0;
    reg [11:0] line_words = 1280;
    wire [31:0] live_crop, rr_data;
    wire [9:0] live_count;
    wire [11:0] live_words;
    wire settled;
    videocap_live_publish publisher (
        .cap_clk(cap_clk), .axi_clk(axi_clk),
        .crop_h(crop_h), .crop_v(crop_v), .line_count(line_count),
        .line_words(line_words),
        .settled(settled), .live_effective_crop(live_crop),
        .live_line_count(live_count), .live_line_words(live_words)
    );
    videocap_stats_read_mux mux (
        .regread_addr(32'h4e), .vcap_live_line_count(live_count),
        .rr_data(rr_data)
    );

    integer polls = 0;
    reg [33:0] snapshots [0:3];
    integer issued = 0;
    integer i;
    reg matched;
    always @(negedge axi_clk) begin
        matched = 0;
        for (i = 0; i <= issued; i = i + 1)
            if ({live_count, live_crop[27:16], live_crop[11:0]} === snapshots[i])
                matched = 1;
        if (!matched)
            $fatal(1, "torn snapshot: count=%h crop=%h", live_count, live_crop);
        if (live_crop[31:28] !== 0 || live_crop[15:12] !== 0)
            $fatal(1, "reserved crop bits changed");
        if (rr_data[31:16] !== {6'b0, live_count} ||
            rr_data[15:0] !== {6'b0, live_count})
            $fatal(1, "Z2 word read or Z3 half has wrong line count: %h", rr_data);
        polls = polls + 1;
    end

    task update;
        input [11:0] h, v;
        input [9:0] n;
        begin
            @(negedge cap_clk);
            issued = issued + 1;
            snapshots[issued] = {n, v, h};
            crop_h = h; crop_v = v; line_count = n;
        end
    endtask

    task await_snapshot;
        input [11:0] h, v;
        input [9:0] n;
        integer cycles;
        begin
            cycles = 0;
            while ((!settled || live_crop !== {4'b0, v, 4'b0, h} ||
                    live_count !== n) && cycles < 200) begin
                @(negedge cap_clk);
                cycles = cycles + 1;
            end
            if (cycles == 200)
                $fatal(1, "publication did not converge to count=%h crop=%h/%h", n, v, h);
        end
    endtask

    initial begin
        snapshots[0] = {10'd0, 12'd26, 12'd188};
        repeat (4) @(negedge axi_clk);
        update(213, 16, 511);
        await_snapshot(213, 16, 511);

        /* Stall the destination with a transfer outstanding; replace both
         * crop axes and cross the 511->512 binary line-count boundary. */
        @(negedge axi_clk); axi_running = 0;
        update(168, 2, 512);
        repeat (12) @(negedge cap_clk);
        if (settled || live_count !== 511 || live_crop !== 32'h001000d5)
            $fatal(1, "in-flight publication became visible or acknowledged early");
        update(4095, 4094, 1023);
        repeat (8) @(negedge cap_clk);
        axi_running = 1;
        await_snapshot(4095, 4094, 1023);
        repeat (8) @(negedge axi_clk);
        $display("RESULT PASS live publication: coherent polling, in-flight updates, Z2 read (%0d polls)", polls);
        $finish;
    end
    initial begin
        #100000;
        $fatal(1, "live publication timeout");
    end
endmodule
