`timescale 1 ns / 1 ps
/*
 * ZZ9000 videocap sampler: doubled-scan (31 kHz) and 24 kHz sources.
 *
 * The AGA / ECS programmable modes - DblPAL, DblNTSC, Euro72, Multiscan
 * (912..1040 capture clocks a line at 28 MHz, one pixel per clock) and
 * Super72 (1232 clocks, two clocks a pixel) - are classified by the
 * measured line period (cap_doubled / cap_short), shown whole rather than
 * as a mis-read PAL / NTSC screen, and their lines are published even
 * though they end long before the 1280th sample.  Numbers are the
 * BeamBender BigBox's measurements on an A4000 (ZZ9000_CAPTURE_TABLES.md,
 * 29 September 2026).
 *
 *   +CASE=0  Euro72 progressive: 976 clocks, 427 lines, filtered path
 *   +CASE=1  Euro72 progressive, full-width path (lines published at
 *            their end, 763 samples in)
 *   +CASE=2  DblPAL interlaced: 1016 clocks, 287 lines a field, the
 *            VSYNC phase alternating by half a line; tall (574 rows)
 *   +CASE=3  Super72: 1232 clocks, 329 lines, short but not doubled -
 *            pairs are kept (two clocks a pixel)
 *   +CASE=4  PAL, 1816 clocks, 312 lines: nothing changes for a 15 kHz
 *            source (the 15 kHz behaviour is the sampler_tb's business;
 *            this only checks the class flags stay clear)
 *
 *   iverilog -g2012 -o doubled.vvp videocap_sampler.v \
 *       videocap_calibration_capture.v videocap_writeback_layout.v \
 *       test/video/xpm_cdc_sim.sv test/video/videocap_doubled_tb.v
 *   vvp doubled.vvp +CASE=0
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
module videocap_doubled_tb;

integer CASE = 0;
integer LINECLKS = 976;
integer LINES = 427;
integer PIXSPAN = 1;          /* capture clocks per source pixel */
integer FULLWIDTH = 0;
integer LACED = 0;
integer HSLOW = 71;           /* the sync pulse, clocks */
integer EXPECT_DOUBLED = 1;
integer EXPECT_SHORT = 1;
integer EXPECT_TALL = 0;
integer EXPECT_NTSC = 1;
integer EXPECT_LACED = 0;
integer EXPECT_CROPH = 168;   /* the doubled-scan default on the filtered
                                 path (213 less the 45-pixel margin) */
integer EXPECT_CROPV = 2;

reg cap_clk = 0;
reg cap_reset = 1;
reg custom_pending = 0;
reg axi_clk = 0;
reg vsync = 1;
reg hsync = 1;
reg [7:0] r = 0, g = 0, b = 0;

always #4.375 cap_clk = ~cap_clk;    /* 114 MHz cap_clk is the real part's;
                                        the period only scales the run */
always #5 axi_clk = ~axi_clk;

reg control_request_event = 0;
reg [31:0] control_request_raw = 0;
wire control_send;
wire [28:0] control_payload;
wire control_received;
wire control_busy;
wire [7:0] control_request_sequence, control_applied_sequence;
wire control_rejected, control_applied_valid;
wire [31:0] control_applied_raw;

videocap_control_source #(.FULLRATE(1)) control (
    .source_clk(axi_clk),
    .request_event(control_request_event),
    .request_raw(control_request_raw),
    .request_token_valid(1'b1),
    .control_received(control_received),
    .control_send(control_send),
    .control_payload(control_payload),
    .busy(control_busy),
    .request_sequence(control_request_sequence),
    .applied_sequence(control_applied_sequence),
    .last_commit_rejected(control_rejected),
    .applied_valid(control_applied_valid),
    .applied_raw(control_applied_raw)
);

wire capture_ready;
wire [10:0] cap_x, cap_y, cap_ymax;
wire cap_interlace, cap_ntsc, cap_doubled, cap_short, cap_tall;
wire cap_x_done, cap_shres, cap_line_toggle, cap_frame_anchor_toggle;
wire cap_write_bank;
wire [9:0] cap_token_y;
wire cap_token_bank;
wire [1:0] detected_standard;
wire [31:0] live_effective_crop;
wire [9:0] live_line_count;
wire [31:0] buf_rdata;

videocap_sampler #(
    .BUF_DEPTH(2048), .RGB_MODE(0), .CSYNC_VSYNC(0), .FULLRATE(1)
) dut (
    .cap_clk(cap_clk), .cap_reset(cap_reset), .capture_ready(capture_ready),
    .axi_resetn(1'b1),
    .cal_arm(1'b0), .cal_address(10'd0), .cal_metadata_address(4'd0),
    .grid_ref(1'b0),
    .vcap_vsync(vsync), .vcap_hsync(hsync),
    .vcap_r(r), .vcap_g(g), .vcap_b(b),
    .ctl_send(control_send), .ctl_payload(control_payload),
    .ctl_received(control_received),
    .ctl_read_full_width(control_applied_raw[2]),
    .detected_standard(detected_standard),
    .live_effective_crop(live_effective_crop),
    .live_line_count(live_line_count),
    .cap_x(cap_x), .cap_y(cap_y), .cap_ymax(cap_ymax),
    .cap_interlace(cap_interlace), .cap_ntsc(cap_ntsc),
    .cap_doubled(cap_doubled), .cap_short(cap_short), .cap_tall(cap_tall),
    .cap_x_done(cap_x_done), .cap_shres(cap_shres),
    .cap_line_toggle(cap_line_toggle),
    .cap_frame_anchor_toggle(cap_frame_anchor_toggle),
    .cap_write_bank(cap_write_bank),
    .cap_token_y(cap_token_y), .cap_token_bank(cap_token_bank),
    .probe_arm_toggle(1'b0), .probe_precrop_raddr(6'd0),
    .axi_clk(axi_clk), .buf_rbank(1'b0), .buf_raddr(12'd0),
    .buf_rdata(buf_rdata)
);

integer checks = 0, errors = 0;
task check_eq;
    input [255:0] name;
    input integer got;
    input integer want;
    begin
        checks = checks + 1;
        if (got !== want) begin
            errors = errors + 1;
            $display("MISMATCH %0s got=%0d want=%0d", name, got, want);
        end
    end
endtask

/* completed-line tokens and the widest line seen, per frame */
integer tokens = 0;
integer max_cap_x = 0;
reg toggle_seen = 0;
always @(posedge cap_clk) begin
    if (cap_line_toggle != toggle_seen) begin
        toggle_seen <= cap_line_toggle;
        tokens = tokens + 1;
    end
    if (cap_x > max_cap_x) max_cap_x = cap_x;
end


integer i, ln;
integer field_lines = 0;      /* lines per field when LACED alternates
                                * N/N+1 (287/288 like real hardware) */

/* One line: the sync pulse, then pixels one every PIXSPAN clocks, a
 * value that changes every pixel so the SuperHires and pairing metrics
 * see edges where a real picture has them.  vsync_at: the clock within
 * the line at which VSYNC falls (-1: not this line). */
task drive_line;
    input integer seed;
    input integer vsync_at;
    integer px;
    begin
        hsync = 0;
        for (i = 0; i < HSLOW; i = i + 1) begin
            if (i == vsync_at) vsync = 0;
            @(posedge cap_clk);
        end
        hsync = 1;
        for (i = HSLOW; i < LINECLKS; i = i + 1) begin
            if (i == vsync_at) vsync = 0;
            px = ((i - HSLOW) / PIXSPAN) + seed;
            r = px[7:0]; g = ~px[7:0]; b = {px[3:0], px[7:4]};
            @(posedge cap_clk);
        end
        r = 0; g = 0; b = 0;
    end
endtask

/* A field: VSYNC falls on the first line (at `phase` clocks in - half a
 * line on the odd fields of a laced source), stays low three lines. */
task drive_field;
    input integer phase;
    begin
        drive_line(0, phase);
        drive_line(1, -1);
        drive_line(2, -1);
        vsync = 1;
        for (ln = 3; ln < field_lines; ln = ln + 1)
            drive_line(ln, -1);
    end
endtask

task drive_field_count;
    input integer phase;
    input integer count;
    begin
        field_lines = count;
        drive_field(phase);
    end
endtask

integer field_no = 0;         /* parity carried across calls: a laced
                                 source alternates every field */
task drive_frames;
    input integer n;
    integer f;
    begin
        for (f = 0; f < n; f = f + 1) begin
            /* Real laced fields alternate their line count by one
             * (287/288 DblPAL) with the VSYNC at the same phase: the
             * parity rule reads the count, the phase may or may not
             * alternate - both must stay interlaced. */
            if (LACED)
                drive_field_count(0,
                    (field_no & 1) ? LINES + 1 : LINES);
            else begin
                field_lines = LINES;
                drive_field(0);
            end
            field_no = field_no + 1;
        end
    end
endtask

initial begin
    repeat (8) @(posedge cap_clk);
    @(negedge cap_clk); cap_reset = 0;
    if ($value$plusargs("CASE=%d", CASE)) ;
    case (CASE)
        1: begin FULLWIDTH = 1; EXPECT_CROPH = 213; EXPECT_CROPV = 16; end
        2: begin LINECLKS = 1016; LINES = 287; LACED = 1; EXPECT_TALL = 1;
                 EXPECT_NTSC = 0; EXPECT_LACED = 1; end
        /* driven progressive here (329 rows: fits 480, so the flag says
         * NTSC); the real Super72 is laced, 658 woven rows, PAL */
        3: begin LINECLKS = 1232; LINES = 329; PIXSPAN = 2; EXPECT_DOUBLED = 0;
                 EXPECT_CROPH = 171; EXPECT_NTSC = 1; end
        /* DblPAL NoLace: 574 constant lines a frame - the VSYNC phase
         * may still alternate, the line-count parity does not: the
         * parity rule must call it progressive (stride-1 rows). */
        5: begin LINECLKS = 1016; LINES = 574; FULLWIDTH = 1;
                 EXPECT_CROPH = 213; EXPECT_CROPV = 16;
                 EXPECT_TALL = 1; EXPECT_NTSC = 0; EXPECT_LACED = 0; end
        4: begin LINECLKS = 1816; LINES = 312; PIXSPAN = 2; HSLOW = 133;
                 EXPECT_DOUBLED = 0; EXPECT_SHORT = 0; EXPECT_NTSC = 0;
                 EXPECT_CROPH = 188; EXPECT_CROPV = 40; end
        default: ;
    endcase
    $display("videocap_doubled_tb CASE %0d: %0d clocks a line, %0d lines, %0s, %0s",
             CASE, LINECLKS, LINES, FULLWIDTH ? "full width" : "filtered",
             LACED ? "laced" : "progressive");

    /* automatic crops, the width as the case says */
    @(negedge axi_clk);
    control_request_raw = (1 << 28) | (1 << 29) | (FULLWIDTH ? (1 << 2) : 0);
    control_request_event = 1;
    @(negedge axi_clk);
    control_request_event = 0;

    /* Reset recovery suppresses the first field; classification then
     * resolves line class, interlace parity, and the woven PAL/NTSC flag
     * on successive boundaries. Settle before measuring two fields. */
    drive_frames(4);
    /* The published standard settles a few fields after the class flags:
     * the interlace-parity rule needs two laced fields, the candidate must
     * then agree twice, and the handshake carries it across.  Wait for
     * the AXI-side value before measuring, so the token counters below
     * only see the two measured fields. */
    begin : wait_published_standard
        integer std_wait;
        for (std_wait = 0; std_wait < 8; std_wait = std_wait + 1) begin
            if (detected_standard == (EXPECT_NTSC ? 2 : 1))
                std_wait = 8;
            else
                drive_frames(1);
        end
    end
    tokens = 0; max_cap_x = 0;
    drive_frames(2);

    check_eq("doubled", cap_doubled, EXPECT_DOUBLED);
    check_eq("short", cap_short, EXPECT_SHORT);
    check_eq("interlace", cap_interlace, EXPECT_LACED);
    check_eq("tall", cap_tall, EXPECT_TALL);
    check_eq("ntsc_flag", cap_ntsc, EXPECT_NTSC);
    /* VCAP_LIVE_STATUS publishes the standard the ARM applies: the coherent
     * AXI-side value must agree with cap_ntsc on every line class (a laced
     * DblPAL field of 287 lines is PAL, a 329-row Super72 frame NTSC). */
    check_eq("published_standard", detected_standard, EXPECT_NTSC ? 2 : 1);
    check_eq("crop_h_in_use", dut.crop_h_local, EXPECT_CROPH);
    check_eq("crop_v_in_use", dut.crop_v_eff, EXPECT_CROPV);
    /* Both live axes must catch up to the class-resolved crop in use. */
    begin : wait_live_crop
        integer crop_wait;
        for (crop_wait = 0; crop_wait < 64; crop_wait = crop_wait + 1) begin
            if (live_effective_crop[11:0] == EXPECT_CROPH[11:0] &&
                    live_effective_crop[27:16] == EXPECT_CROPV[11:0])
                crop_wait = 64;
            else
                @(posedge axi_clk);
        end
    end
    check_eq("live_effective_crop_h", live_effective_crop[11:0], EXPECT_CROPH);
    check_eq("live_effective_crop_v", live_effective_crop[27:16], EXPECT_CROPV);
    /* a laced field's VSYNC lands half a line later every other field:
     * counted from VSYNC to VSYNC the fields read LINES + 1 and LINES - 1 */
    check_eq("field_lines", (cap_ymax == LINES) ||
             (LACED && (cap_ymax == LINES + 1 || cap_ymax == LINES - 1)), 1);
    /* every visible line published once a field: LINES less the crop,
     * the sentinel row and the field's first (partial) line; two fields
     * (a laced pair sums to the same, give or take the half line) */
    if (LACED)
        check_eq("tokens_two_fields",
                 (tokens >= 2 * (LINES - EXPECT_CROPV - 2) - 1) &&
                 (tokens <= 2 * (LINES - EXPECT_CROPV - 2) + 1), 1);
    else
        check_eq("tokens_two_fields", tokens, 2 * (LINES - EXPECT_CROPV - 2));
    /* the stored width, in samples from the crop origin to the next
     * accepted edge: every sample of a doubled line (the 640 pixels
     * plus the blank to the line's end), one per pair otherwise */
    if (EXPECT_DOUBLED)
        check_eq("stored_samples_per_line", max_cap_x,
                 LINECLKS - EXPECT_CROPH - 1);
    else if (CASE == 3)
        check_eq("stored_pairs_per_line",
                 (max_cap_x >= (LINECLKS - EXPECT_CROPH - 2) / 2) &&
                 (max_cap_x <= (LINECLKS - EXPECT_CROPH) / 2), 1);
    if (EXPECT_DOUBLED && FULLWIDTH)
        check_eq("full_width_line_ended_before_1280", max_cap_x < 1280, 1);
    /* A short-line source must never classify as SuperHires: the
     * pair-difference metric is a 15 kHz property. */
    if (EXPECT_SHORT)
        check_eq("short_source_not_shres", cap_shres, 0);

    $display("field lines %0d, tokens %0d, widest %0d", cap_ymax, tokens, max_cap_x);
    /* A custom commit on a detected short-line source must replace the
     * automatic pair, and ACK must not race its AXI-visible snapshot. */
    @(negedge axi_clk);
    custom_pending = 1;
    control_request_raw = (21 << 16) | (300 << 4) |
                          (FULLWIDTH ? (1 << 2) : 0);
    control_request_event = 1;
    @(negedge axi_clk);
    control_request_event = 0;
    drive_frames(2);
    check_eq("custom_crop_commit_complete", control_busy, 0);
    check_eq("custom_crop_readback", live_effective_crop, (21 << 16) | 300);
    check_eq("live_frame_line_count", live_line_count, cap_ymax[9:0]);
    if (errors == 0)
        $display("RESULT PASS checks=%0d", checks);
    else
        $display("RESULT FAIL checks=%0d errors=%0d", checks, errors);
    $finish;
end

always @(posedge control_received) begin
    if (custom_pending && live_effective_crop !== ((21 << 16) | 300))
        $fatal(1, "control ACK preceded coherent live crop readback");
end

endmodule
