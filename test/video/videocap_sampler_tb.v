`timescale 1ns / 1ps
/*
 * Functional testbench for videocap_sampler.v.
 *
 * Generates a PAL-shaped active-low sync raster and paints each Amiga pixel
 * across a configurable number of capture clocks.  The same stimulus covers
 * lores (PIXSPAN=4), hires (PIXSPAN=2), and SuperHires (PIXSPAN=1).
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

module videocap_sampler_tb;

integer PIXSPAN;
integer SAMPLEMODE;
integer FULLWIDTH;
integer CROPH;
integer CROPV;
integer LINES;
integer LINECLKS;

/*
 * The sampler recognizes HSYNC through a six-stage synchronizer and registers
 * RGB before storing it.  With this stimulus ordering, capture sample zero is
 * input sample four.  Keep that fixed phase explicit in the oracle so crop_h
 * remains measured in sampler clocks rather than testbench loop iterations.
 */
localparam integer CAPTURE_INPUT_OFFSET = 4;

reg cap_clk = 0;
reg axi_clk = 0;
reg vsync = 1;
reg hsync = 1;
reg [7:0] r = 0;
reg [7:0] g = 0;
reg [7:0] b = 0;

reg [11:0] buf_raddr = 0;
wire [31:0] buf_rdata;
wire [10:0] cap_x;
wire [10:0] cap_y;
wire [10:0] cap_ymax;
wire cap_interlace;
wire cap_ntsc;
wire cap_x_done;
wire cap_shres;

videocap_sampler #(
    .BUF_DEPTH(2048),
    .RGB_MODE(0),
    .CSYNC_VSYNC(0),
    .FULLRATE(1)
) dut (
    .cap_clk(cap_clk),
    .vcap_vsync(vsync),
    .vcap_hsync(hsync),
    .vcap_r(r),
    .vcap_g(g),
    .vcap_b(b),
    .ctl_sample_mode(SAMPLEMODE[1:0]),
    .ctl_full_width(FULLWIDTH[0]),
    .ctl_crop_h(CROPH[11:0]),
    .ctl_crop_v(CROPV[11:0]),
    .cap_x(cap_x),
    .cap_y(cap_y),
    .cap_ymax(cap_ymax),
    .cap_interlace(cap_interlace),
    .cap_ntsc(cap_ntsc),
    .cap_x_done(cap_x_done),
    .cap_shres(cap_shres),
    .axi_clk(axi_clk),
    .buf_raddr(buf_raddr),
    .buf_rdata(buf_rdata)
);

always #17.6 cap_clk = ~cap_clk; /* 28.37 MHz */
always #5.0 axi_clk = ~axi_clk; /* 100 MHz */

integer errors = 0;
integer checks = 0;

task check_eq;
    input [255:0] name;
    input [31:0] got;
    input [31:0] want;
    begin
        checks = checks + 1;
        if (got !== want) begin
            errors = errors + 1;
            $display("MISMATCH %0s got=%08x want=%08x", name, got, want);
        end
    end
endtask

task buf_read;
    input [11:0] addr;
    output [31:0] data;
    begin
        @(posedge axi_clk);
        buf_raddr <= addr;
        @(posedge axi_clk);
        @(posedge axi_clk);
        data = buf_rdata;
    end
endtask

task drive_line;
    input integer pattern_seed;
    integer i;
    integer px;
    begin
        hsync = 0;
        for (i = 0; i < 67; i = i + 1)
            @(posedge cap_clk);
        hsync = 1;
        for (i = 0; i < LINECLKS - 67; i = i + 1) begin
            px = (i / PIXSPAN) + pattern_seed;
            r = px[7:0];
            g = ~px[7:0];
            b = {px[3:0], px[7:4]};
            @(posedge cap_clk);
        end
    end
endtask

task drive_field;
    input integer seed;
    integer ln;
    begin
        vsync = 0;
        drive_line(seed);
        drive_line(seed);
        vsync = 1;
        for (ln = 0; ln < LINES; ln = ln + 1)
            drive_line(seed + ln);
    end
endtask

integer sample_idx;
integer pix_even;
integer pix_odd;
integer line_seed;
integer k;
reg [31:0] got;
reg [7:0] want_r;
reg [7:0] want_g;
reg [7:0] want_b;
reg [7:0] even_r;
reg [7:0] even_g;
reg [7:0] even_b;
reg [7:0] odd_r;
reg [7:0] odd_g;
reg [7:0] odd_b;

initial begin
    PIXSPAN = 2;
    SAMPLEMODE = 0;
    FULLWIDTH = 0;
    CROPH = 188;
    CROPV = 26;
    LINES = 40;
    LINECLKS = 1816;
    if ($value$plusargs("PIXSPAN=%d", PIXSPAN)) ;
    if ($value$plusargs("SAMPLEMODE=%d", SAMPLEMODE)) ;
    if ($value$plusargs("FULLWIDTH=%d", FULLWIDTH)) ;
    if ($value$plusargs("CROPH=%d", CROPH)) ;
    if ($value$plusargs("CROPV=%d", CROPV)) ;
    if ($value$plusargs("LINES=%d", LINES)) ;
    if ($value$plusargs("LINECLKS=%d", LINECLKS)) ;

    repeat (10) @(posedge cap_clk);
    drive_field(0);
    drive_field(0);

    /* The final active raster line remains in the line buffer. */
    line_seed = LINES - 1;
    for (k = 4; k < 32; k = k + 1) begin
        buf_read(k[11:0], got);
        sample_idx = CROPH + k * (FULLWIDTH ? 1 : 2)
                     + CAPTURE_INPUT_OFFSET;
        pix_even = sample_idx / PIXSPAN + line_seed;
        pix_odd = (sample_idx + 1) / PIXSPAN + line_seed;
        even_r = pix_even[7:0];
        even_g = ~pix_even[7:0];
        even_b = {pix_even[3:0], pix_even[7:4]};
        odd_r = pix_odd[7:0];
        odd_g = ~pix_odd[7:0];
        odd_b = {pix_odd[3:0], pix_odd[7:4]};
        if (FULLWIDTH || SAMPLEMODE == 1) begin
            want_r = even_r;
            want_g = even_g;
            want_b = even_b;
        end else if (SAMPLEMODE == 2) begin
            want_r = odd_r;
            want_g = odd_g;
            want_b = odd_b;
        end else begin
            want_r = ({1'b0, even_r} + {1'b0, odd_r} + 9'd1) >> 1;
            want_g = ({1'b0, even_g} + {1'b0, odd_g} + 9'd1) >> 1;
            want_b = ({1'b0, even_b} + {1'b0, odd_b} + 9'd1) >> 1;
        end
        check_eq("entry", got[23:0], {want_r, want_g, want_b});
    end

    if (errors == 0)
        $display("RESULT PASS checks=%0d", checks);
    else
        $display("RESULT FAIL checks=%0d errors=%0d", checks, errors);
    $finish;
end

endmodule
