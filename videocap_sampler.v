`timescale 1 ns / 1 ps
/*
 * ZZ9000 Amiga native video capture sampler.
 *
 * Extracted from mntzorro.v so the capture state machine can be simulated.
 * Variant behavior is supplied through parameters because preprocessor
 * definitions are not reliably shared between Vivado compilation units.
 *
 * Copyright (C) 2019-2026, Lucie L. Hartmann <lucie@mntre.com>
 * Copyright (C) 2026,      Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

module videocap_sampler #(
    parameter integer BUF_DEPTH   = 2048,
    parameter integer RGB_MODE    = 0,
    parameter integer CSYNC_VSYNC = 0,
    parameter integer FULLRATE    = 0
) (
    input  wire        cap_clk,
    input  wire        vcap_vsync,
    input  wire        vcap_hsync,
    input  wire [7:0]  vcap_r,
    input  wire [7:0]  vcap_g,
    input  wire [7:0]  vcap_b,

    input  wire [1:0]  ctl_sample_mode,
    input  wire        ctl_full_width,
    input  wire [11:0] ctl_crop_h,
    input  wire [11:0] ctl_crop_v,

    output reg  [10:0] cap_x,
    output reg  [10:0] cap_y,
    output reg  [10:0] cap_ymax,
    output reg         cap_interlace,
    output reg         cap_ntsc,
    output reg         cap_x_done,
    output reg         cap_shres,

    input  wire        axi_clk,
    input  wire [11:0] buf_raddr,
    output wire [31:0] buf_rdata
);

reg [6:0] hs = 0;
reg [6:0] vs = 0;
reg [23:0] rgbin = 0;
reg [10:0] sample_x = 0;
reg [10:0] raw_y = 0;
reg lace_field = 0;
reg next_lace_field = 0;
reg [3:0] shortlines = 0;
reg [7:0] hs_pulse_width = 0;
reg [10:0] vsync_x = 0;

reg [31:0] linebuf [0:BUF_DEPTH-1];
reg [31:0] buf_rdata_r;
assign buf_rdata = buf_rdata_r;

always @(posedge axi_clk)
    buf_rdata_r <= linebuf[buf_raddr];

localparam [10:0] INTERLACE_PHASE_DELTA = 11'h080;
wire [10:0] vsync_phase_abs_delta =
    (cap_x > vsync_x) ? (cap_x - vsync_x) : (vsync_x - cap_x);
wire [10:0] vsync_phase_delta =
    (vsync_phase_abs_delta > 11'h200) ?
    (11'h3ff - vsync_phase_abs_delta + 1'b1) :
    vsync_phase_abs_delta;

wire frame_sync = (CSYNC_VSYNC != 0) ?
    (hs[6:1] == 6'b000111 && hs_pulse_width >= 8'd128) :
    (vs[6:1] == 6'b111000);
wire line_sync = (hs[6:1] == 6'b000111);

/*
 * The control interface always expresses crop_h in 28 MHz samples. Denise
 * adapters retain the 14 MHz front end, so one local capture clock consumes
 * two control units there. This keeps the universal default (188) equivalent
 * to the historical 94-clock crop without requiring a firmware variant.
 */
wire [11:0] crop_h_local = (FULLRATE != 0) ?
    ctl_crop_h : {1'b0, ctl_crop_h[11:1]};

reg half = 0;
reg [23:0] rgb_prev = 0;
wire filter_pairs = (FULLRATE != 0) && !ctl_full_width;

wire [8:0] avg_r_sum = {1'b0, rgb_prev[23:16]} +
                       {1'b0, rgbin[23:16]} + 9'd1;
wire [8:0] avg_g_sum = {1'b0, rgb_prev[15:8]} +
                       {1'b0, rgbin[15:8]} + 9'd1;
wire [8:0] avg_b_sum = {1'b0, rgb_prev[7:0]} +
                       {1'b0, rgbin[7:0]} + 9'd1;
wire [23:0] rgb_average = {avg_r_sum[8:1], avg_g_sum[8:1],
                           avg_b_sum[8:1]};
wire [23:0] filtered_sample =
    (ctl_sample_mode == 2'd1) ? rgb_prev :
    (ctl_sample_mode == 2'd2) ? rgbin : rgb_average;

reg [15:0] diff_count = 0;

always @(posedge cap_clk) begin
    vs <= {vs[5:0], vcap_vsync};
    hs <= {hs[5:0], vcap_hsync};

    if (RGB_MODE == 1)
        rgbin <= {vcap_r[3:0], vcap_r[3:0],
                  vcap_g[3:0], vcap_g[3:0],
                  vcap_b[3:0], vcap_b[3:0]};
    else if (RGB_MODE == 2)
        rgbin <= {vcap_r[7:4], vcap_r[7:4],
                  vcap_g[7:4], vcap_g[7:4],
                  vcap_b[7:4], vcap_b[7:4]};
    else
        rgbin <= {vcap_r, vcap_g, vcap_b};

    if (hs == 0) begin
        if (hs_pulse_width < 8'hff)
            hs_pulse_width <= hs_pulse_width + 1'b1;
    end else if (hs == 7'b0111111) begin
        /* Preserve the legacy six-high-sample pulse-width reset. */
        hs_pulse_width <= 0;
    end

    if (frame_sync) begin
        if (cap_ymax >= 11'h190)
            cap_interlace <= 0;
        else if (CSYNC_VSYNC != 0)
            cap_interlace <= (next_lace_field != lace_field);
        else
            cap_interlace <=
                (vsync_phase_delta >= INTERLACE_PHASE_DELTA);

        if (CSYNC_VSYNC != 0)
            lace_field <= next_lace_field;
        else begin
            vsync_x <= cap_x;
            lace_field <= cap_ymax[0];
        end

        if (cap_ymax >= 11'h190)
            cap_ntsc <= (cap_ymax >= 11'h23a) ? 1'b0 : 1'b1;
        else
            cap_ntsc <= (cap_ymax >=
                ((CSYNC_VSYNC != 0) ? 11'h130 : 11'h138)) ? 1'b0 : 1'b1;

        raw_y <= 0;
        cap_y <= cap_interlace ?
            {10'b0, (CSYNC_VSYNC != 0) ? next_lace_field : lace_field} :
            11'b0;

        cap_shres <= (diff_count > 16'd64);
        diff_count <= 0;

        if (raw_y != 0)
            cap_ymax <= raw_y;
    end else if (line_sync) begin
        cap_x <= 0;
        sample_x <= 0;
        half <= 0;

        if (CSYNC_VSYNC != 0) begin
            if (hs_pulse_width < 8'h20) begin
                shortlines <= shortlines + 1'b1;
                if (shortlines == 0)
                    next_lace_field <= (cap_x >= 11'h200);
            end else begin
                shortlines <= 0;
            end
        end

        if (raw_y > ctl_crop_v[10:0]) begin
            if (cap_interlace)
                cap_y <= cap_y + 2'b10;
            else
                cap_y <= cap_y + 1'b1;
        end
        raw_y <= raw_y + 1'b1;
    end else begin
        sample_x <= sample_x + 1'b1;

        if ({1'b0, sample_x} < crop_h_local) begin
            half <= 0;
        end else if (filter_pairs) begin
            if (!half) begin
                rgb_prev <= rgbin;
                half <= 1;
            end else begin
                half <= 0;
                if (cap_x > 2)
                    linebuf[cap_x] <= {8'b0, filtered_sample};
                else
                    linebuf[cap_x] <= 32'b0;
                cap_x <= cap_x + 1'b1;
                if ((rgbin !== rgb_prev) && diff_count != 16'hffff)
                    diff_count <= diff_count + 1'b1;
            end
        end else begin
            if (cap_x > 2)
                linebuf[cap_x] <= {8'b0, rgbin};
            else
                linebuf[cap_x] <= 32'b0;
            cap_x <= cap_x + 1'b1;
        end
    end

    cap_x_done <=
        (cap_x > (ctl_full_width ? 11'h400 : 11'h200));
end

endmodule
