`timescale 1 ns / 1 ps
/*
 * Composite 15 kHz equalisation burst.  The field ends on half-line
 * pulses (454 clocks at 14 MHz, 908 at 28 MHz), which sit inside the
 * doubled/short window.  Classification must keep the active-video
 * period across that burst.
 *
 *   iverilog -g2012 -o csync_class.vvp videocap_sampler.v \
 *       videocap_calibration_capture.v videocap_writeback_layout.v \
 *       test/video/xpm_cdc_sim.sv test/video/videocap_csync_class_tb.v
 *   vvp csync_class.vvp
 */
module csync_class_one #(
    parameter integer FULLRATE = 0,
    parameter integer LINE_CLKS = 908,
    parameter integer HALF_CLKS = 454
) (
    input wire cap_clk,
    input wire axi_clk,
    input wire cap_reset,
    output wire cap_doubled,
    output wire cap_short,
    output wire cap_ntsc,
    output wire [10:0] cap_ymax
);
    reg hsync = 1;

    videocap_sampler #(
        .CSYNC_VSYNC(1), .FULLRATE(FULLRATE), .BUF_DEPTH(256)
    ) dut (
        .cap_clk(cap_clk), .cap_reset(cap_reset),
        .axi_resetn(1'b1), .axi_clk(axi_clk),
        .cal_arm(1'b0), .cal_address(10'd0), .cal_metadata_address(4'd0),
        .grid_ref(1'b0), .vcap_vsync(1'b1),
        .vcap_r(8'd0), .vcap_g(8'd0), .vcap_b(8'd0),
        .ctl_send(1'b0), .ctl_payload(29'd0), .ctl_read_full_width(1'b0),
        .probe_arm_toggle(1'b0), .probe_precrop_raddr(6'd0),
        .buf_rbank(1'b0), .buf_raddr(12'd0),
        .vcap_hsync(hsync),
        .cap_ntsc(cap_ntsc), .cap_doubled(cap_doubled),
        .cap_short(cap_short), .cap_ymax(cap_ymax)
    );

    task pulse;
        input integer low_clks;
        input integer period;
        integer k;
        begin
            hsync = 0;
            for (k = 0; k < low_clks; k = k + 1)
                @(posedge cap_clk);
            hsync = 1;
            for (k = low_clks; k < period; k = k + 1)
                @(posedge cap_clk);
        end
    endtask

    task field;
        integer n;
        begin
            for (n = 0; n < 312; n = n + 1)
                pulse(LINE_CLKS / 16, LINE_CLKS);
            for (n = 0; n < 4; n = n + 1)
                pulse(20, HALF_CLKS);
            /* Wide composite field mark.  The class edge inherits the
             * half-line interval that ended on the previous edge. */
            pulse(160, HALF_CLKS + 160);
        end
    endtask

    integer f;
    initial begin
        @(negedge cap_reset);
        for (f = 0; f < 4; f = f + 1)
            field;
    end
endmodule

module videocap_csync_class_tb;
    reg cap_clk = 0;
    reg axi_clk = 0;
    reg cap_reset = 1;
    always #4.375 cap_clk = ~cap_clk;
    always #5 axi_clk = ~axi_clk;

    wire doubled_14, short_14, ntsc_14;
    wire doubled_28, short_28, ntsc_28;
    wire [10:0] ymax_14, ymax_28;

    csync_class_one #(.FULLRATE(0), .LINE_CLKS(908), .HALF_CLKS(454)) denise (
        .cap_clk(cap_clk), .axi_clk(axi_clk), .cap_reset(cap_reset),
        .cap_doubled(doubled_14), .cap_short(short_14),
        .cap_ntsc(ntsc_14), .cap_ymax(ymax_14)
    );
    csync_class_one #(.FULLRATE(1), .LINE_CLKS(1816), .HALF_CLKS(908)) fullrate (
        .cap_clk(cap_clk), .axi_clk(axi_clk), .cap_reset(cap_reset),
        .cap_doubled(doubled_28), .cap_short(short_28),
        .cap_ntsc(ntsc_28), .cap_ymax(ymax_28)
    );

    integer errors = 0;
    task check;
        input [255:0] name;
        input integer got;
        input integer want;
        begin
            if (got !== want) begin
                errors = errors + 1;
                $display("MISMATCH %0s got=%0d want=%0d", name, got, want);
            end
        end
    endtask

    initial begin
        repeat (8) @(posedge cap_clk);
        @(negedge cap_clk);
        cap_reset = 0;
        /* Each instance drives four fields on its own hsync.  Wait until
         * both have finished (the 28 MHz field is the longer one). */
        repeat (4 * (312 * 1816 + 4 * 908 + 908 + 160) + 64) @(posedge cap_clk);
        check("denise_not_doubled", doubled_14, 0);
        check("denise_not_short", short_14, 0);
        check("denise_pal", ntsc_14, 0);
        check("fullrate_not_doubled", doubled_28, 0);
        check("fullrate_not_short", short_28, 0);
        check("fullrate_pal", ntsc_28, 0);
        $display("ymax denise=%0d fullrate=%0d", ymax_14, ymax_28);
        if (errors == 0)
            $display("RESULT PASS");
        else
            $display("RESULT FAIL errors=%0d", errors);
        $finish;
    end
endmodule
