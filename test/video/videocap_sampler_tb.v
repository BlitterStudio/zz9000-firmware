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

module videocap_sampler_tb #(
    parameter integer DEFAULT_PIXSPAN = 2,
    parameter integer DEFAULT_SAMPLEMODE = 0,
    parameter integer DEFAULT_FULLWIDTH = 0,
    parameter integer DEFAULT_CROPH = 188,
    parameter integer DEFAULT_CROPV = 26
);

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
reg buf_rbank = 0;
reg probe_arm_toggle = 0;
wire [31:0] buf_rdata;
wire [31:0] legacy_buf_rdata;
wire [10:0] cap_x;
wire [10:0] cap_y;
wire [10:0] cap_ymax;
wire cap_interlace;
wire cap_ntsc;
wire cap_x_done;
wire cap_shres;
wire cap_line_toggle;
wire cap_write_bank;
wire probe_arm_seen;
wire probe_valid;
wire [511:0] probe_data;
wire [9:0] probe_line;
wire [11:0] probe_source_x;
wire [31:0] probe_context;
wire [31:0] probe_config;
wire probe_precrop_valid;
wire [31:0] probe_precrop_context;
reg [5:0] probe_precrop_raddr = 0;
wire [31:0] probe_precrop_rdata;
wire legacy_cap_shres;
reg layout_full_width = 0;
reg [11:0] layout_source_x = 0;
wire [11:0] layout_dest_x;
reg [1279:0] layout_seen;
integer layout_k;

videocap_writeback_layout #(
    .LINE_WIDTH(1280),
    .ROTATE_PIXELS(0)
) writeback_layout (
    .full_width(layout_full_width),
    .source_x(layout_source_x),
    .dest_x(layout_dest_x)
);

videocap_sampler #(
    .BUF_DEPTH(2048),
    .RGB_MODE(0),
    .CSYNC_VSYNC(0),
    .FULLRATE(1),
    .PROBE_LINE(0),
    .PROBE_SOURCE_X(32)
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
    .cap_line_toggle(cap_line_toggle),
    .cap_write_bank(cap_write_bank),
    .probe_arm_toggle(probe_arm_toggle),
    .probe_arm_seen(probe_arm_seen),
    .probe_valid(probe_valid),
    .probe_data(probe_data),
    .probe_line(probe_line),
    .probe_source_x(probe_source_x),
    .probe_context(probe_context),
    .probe_config(probe_config),
    .probe_precrop_valid(probe_precrop_valid),
    .probe_precrop_context(probe_precrop_context),
    .probe_precrop_raddr(probe_precrop_raddr),
    .probe_precrop_rdata(probe_precrop_rdata),
    .axi_clk(axi_clk),
    .buf_rbank(buf_rbank),
    .buf_raddr(buf_raddr),
    .buf_rdata(buf_rdata)
);

/* Denise-adapter reference: crop_h remains expressed in 28 MHz units even
 * though this instance preserves the historical 14 MHz capture front end. */
videocap_sampler #(
    .BUF_DEPTH(2048),
    .RGB_MODE(0),
    .CSYNC_VSYNC(0),
    .FULLRATE(0)
) legacy_dut (
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
    .cap_x(),
    .cap_y(),
    .cap_ymax(),
    .cap_interlace(),
    .cap_ntsc(),
    .cap_x_done(),
    .cap_shres(legacy_cap_shres),
    .cap_line_toggle(),
    .cap_write_bank(),
    .probe_arm_toggle(1'b0),
    .probe_arm_seen(),
    .probe_valid(),
    .probe_data(),
    .probe_line(),
    .probe_source_x(),
    .probe_context(),
    .probe_config(),
    .probe_precrop_valid(),
    .probe_precrop_context(),
    .probe_precrop_raddr(6'd0),
    .probe_precrop_rdata(),
    .axi_clk(axi_clk),
    .buf_rbank(1'b0),
    .buf_raddr(buf_raddr),
    .buf_rdata(legacy_buf_rdata)
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

task legacy_buf_read;
    input [11:0] addr;
    output [31:0] data;
    begin
        @(posedge axi_clk);
        buf_raddr <= addr;
        @(posedge axi_clk);
        @(posedge axi_clk);
        data = legacy_buf_rdata;
    end
endtask

task check_full_width_bank_entry;
    input completed_bank;
    input [11:0] addr;
    input integer pattern_seed;
    reg [31:0] data;
    integer input_sample;
    integer pixel;
    reg [7:0] expected_r;
    reg [7:0] expected_g;
    reg [7:0] expected_b;
    begin
        buf_rbank = completed_bank;
        buf_read(addr, data);
        input_sample = CROPH + addr + CAPTURE_INPUT_OFFSET;
        pixel = input_sample / PIXSPAN + pattern_seed;
        expected_r = pixel[7:0];
        expected_g = ~pixel[7:0];
        expected_b = {pixel[3:0], pixel[7:4]};
        check_eq("completed_bank_entry", data[23:0],
                 {expected_r, expected_g, expected_b});
    end
endtask

task check_completed_bank_during_next_line;
    input completed_bank;
    input integer pattern_seed;
    begin
        wait (hsync == 0);
        wait (cap_x < 16);
        wait (cap_x >= 900);

        checks = checks + 1;
        if (cap_write_bank === completed_bank) begin
            errors = errors + 1;
            $display("MISMATCH capture overwrites completed bank=%0d",
                     completed_bank);
        end

        check_full_width_bank_entry(completed_bank, 12'd0, pattern_seed);
        check_full_width_bank_entry(completed_bank, 12'd640, pattern_seed);
        check_full_width_bank_entry(completed_bank, 12'd900, pattern_seed);
    end
endtask

integer first_full_width_ready_x;
integer full_width_ready_checked;
integer full_width_completed_lines;
reg last_completed_bank;
integer last_frame_sync_x;
integer last_frame_phase_abs_delta;
integer last_frame_phase_changed;

always @(posedge cap_clk) begin
    if (dut.frame_sync) begin
        last_frame_sync_x = dut.phase_x;
        last_frame_phase_abs_delta = dut.vsync_phase_abs_delta;
        last_frame_phase_changed = dut.vsync_phase_changed;
    end
end

task drive_line;
    input integer pattern_seed;
    integer i;
    integer px;
    reg line_toggle_before;
    begin
        line_toggle_before = cap_line_toggle;
        hsync = 0;
        for (i = 0; i < 67; i = i + 1)
            @(posedge cap_clk);
        hsync = 1;
        for (i = 0; i < LINECLKS - 67; i = i + 1) begin
            /* Toggle aggressively after the 1280-sample capture window.
             * Blanking activity must not make hires or lores look like
             * SuperHires content. */
            if (i >= CROPH + 1300)
                px = (i[0] != 0) ? 8'hff : 8'h00;
            else
                px = (i / PIXSPAN) + pattern_seed;
            r = px[7:0];
            g = ~px[7:0];
            b = {px[3:0], px[7:4]};
            @(posedge cap_clk);

            if (FULLWIDTH && !full_width_ready_checked &&
                    first_full_width_ready_x < 0 && cap_x_done)
                first_full_width_ready_x = cap_x;
        end

        if (FULLWIDTH && !full_width_ready_checked) begin
            checks = checks + 1;
            if (first_full_width_ready_x < 1280) begin
                errors = errors + 1;
                $display("MISMATCH full_width_ready_x got=%0d expected>=1280",
                         first_full_width_ready_x);
            end
            full_width_ready_checked = 1;
        end

        if (FULLWIDTH && vsync) begin
            checks = checks + 1;
            if (cap_line_toggle === line_toggle_before) begin
                errors = errors + 1;
                $display("MISMATCH full_width_completion_token did not toggle");
            end

            if (full_width_completed_lines > 0) begin
                checks = checks + 1;
                if (cap_write_bank === last_completed_bank) begin
                    errors = errors + 1;
                    $display("MISMATCH full_width_bank did not alternate bank=%0d",
                             cap_write_bank);
                end
            end
            last_completed_bank = cap_write_bank;
            full_width_completed_lines = full_width_completed_lines + 1;
        end
    end
endtask

task drive_field;
    input integer seed;
    input integer check_vertical;
    integer ln;
    reg completed_bank_before_line;
    begin
        vsync = 0;
        drive_line(seed);
        drive_line(seed);
        vsync = 1;
        for (ln = 0; ln < LINES; ln = ln + 1) begin
            if (FULLWIDTH && check_vertical && ln == 2) begin
                completed_bank_before_line = cap_write_bank;
                fork
                    drive_line(seed + ln);
                    check_completed_bank_during_next_line(
                        completed_bank_before_line, seed + ln - 1);
                join
            end else begin
                drive_line(seed + ln);
            end

            /* The two VSYNC lines have already advanced raw_y before the
             * active raster starts.  Observe cap_y only in the second field,
             * after each complete driven line, so the line-sync update has
             * crossed the synchronous sampler boundary.  This two-field
             * stimulus is classified as interlaced, so use the sampler's
             * reported field stride rather than assuming one output row. */
            if (check_vertical && ln == CROPV - 2)
                check_eq("crop_v_before", cap_y, 0);
            if (check_vertical && ln == CROPV - 1)
                check_eq("crop_v_origin", cap_y,
                         cap_interlace ? 2 : 1);
        end

        if (check_vertical && LINES >= CROPV)
            check_eq("crop_v_extent", cap_y,
                     (LINES - CROPV + 1) * (cap_interlace ? 2 : 1));
    end
endtask

/* Drive a field whose VSYNC falling edge starts at a chosen horizontal
 * phase after HSYNC.  Video-slot machines sample at 28.37 MHz, so the two
 * PAL interlace phases are separated by roughly half of LINECLKS (~908
 * clocks), not by the 14 MHz half-line used by Denise adapters. */
task drive_field_with_vsync_phase;
    input integer seed;
    input integer vsync_phase;
    integer i;
    integer px;
    integer ln;
    begin
        vsync = 1;
        hsync = 0;
        for (i = 0; i < 67; i = i + 1)
            @(posedge cap_clk);
        hsync = 1;
        for (i = 0; i < LINECLKS - 67; i = i + 1) begin
            if (i == vsync_phase)
                vsync = 0;
            px = (i / PIXSPAN) + seed;
            r = px[7:0];
            g = ~px[7:0];
            b = {px[3:0], px[7:4]};
            @(posedge cap_clk);
        end

        /* Keep VSYNC asserted for a second line, matching drive_field. */
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
reg interlace_field_parity;

initial begin
    PIXSPAN = DEFAULT_PIXSPAN;
    SAMPLEMODE = DEFAULT_SAMPLEMODE;
    FULLWIDTH = DEFAULT_FULLWIDTH;
    CROPH = DEFAULT_CROPH;
    CROPV = DEFAULT_CROPV;
    LINES = 40;
    LINECLKS = 1816;
    first_full_width_ready_x = -1;
    full_width_ready_checked = 0;
    full_width_completed_lines = 0;
    last_completed_bank = 0;
    if ($value$plusargs("PIXSPAN=%d", PIXSPAN)) ;
    if ($value$plusargs("SAMPLEMODE=%d", SAMPLEMODE)) ;
    if ($value$plusargs("FULLWIDTH=%d", FULLWIDTH)) ;
    if ($value$plusargs("CROPH=%d", CROPH)) ;
    if ($value$plusargs("CROPV=%d", CROPV)) ;
    if ($value$plusargs("LINES=%d", LINES)) ;
    if ($value$plusargs("LINECLKS=%d", LINECLKS)) ;

    repeat (10) @(posedge cap_clk);
    probe_arm_toggle = 1;

    /* Crop 292 supplies the formerly discarded 64-sample prefix directly,
     * so full-width placement no longer rotates late-line blanking into the
     * start of the destination row. */
    layout_full_width = 0;
    layout_source_x = 12'd1216;
    #1 check_eq("filtered_writeback_identity", layout_dest_x, 12'd1216);

    layout_full_width = 1;
    layout_source_x = 12'd0;
    #1 check_eq("full_width_head", layout_dest_x, 12'd0);
    layout_source_x = 12'd1215;
    #1 check_eq("full_width_mid", layout_dest_x, 12'd1215);
    layout_source_x = 12'd1216;
    #1 check_eq("full_width_after_mid", layout_dest_x, 12'd1216);
    layout_source_x = 12'd1279;
    #1 check_eq("full_width_tail", layout_dest_x, 12'd1279);

    layout_seen = 1280'b0;
    for (layout_k = 0; layout_k < 1280; layout_k = layout_k + 1) begin
        layout_source_x = layout_k[11:0];
        #1;
        checks = checks + 1;
        if (layout_dest_x >= 1280 || layout_seen[layout_dest_x]) begin
            errors = errors + 1;
            $display("MISMATCH layout_bijection source=%0d dest=%0d",
                     layout_k, layout_dest_x);
        end else begin
            layout_seen[layout_dest_x] = 1'b1;
        end
    end

    drive_field(0, 0);
    drive_field(0, 1);

    check_eq("cap_shres", cap_shres, (PIXSPAN == 1));
    check_eq("legacy_cap_shres", legacy_cap_shres, 0);

    if (FULLWIDTH) begin
        check_eq("probe_arm_seen", probe_arm_seen, probe_arm_toggle);
        check_eq("probe_valid", probe_valid, 1);
        check_eq("probe_line", probe_line, 0);
        check_eq("probe_source_x", probe_source_x, 32);
        check_eq("probe_full_width", probe_config[24], 1);
        for (k = 0; k < 16; k = k + 1) begin
            got = probe_data[k * 32 +: 32];
            sample_idx = CROPH + 32 + k + CAPTURE_INPUT_OFFSET;
            pix_even = sample_idx / PIXSPAN;
            want_r = pix_even[7:0];
            want_g = ~pix_even[7:0];
            want_b = {pix_even[3:0], pix_even[7:4]};
            check_eq("probe_word", got[23:0],
                     {want_r, want_g, want_b});
        end

        check_eq("probe_precrop_valid", probe_precrop_valid, 1);
        check_eq("probe_precrop_sample_x",
                 probe_precrop_context[10:0], CROPH - 64);
        for (k = 0; k < 64; k = k + 1) begin
            probe_precrop_raddr = k[5:0];
            #1 got = probe_precrop_rdata;
            sample_idx = CROPH - 64 + k + CAPTURE_INPUT_OFFSET;
            pix_even = sample_idx / PIXSPAN;
            want_r = pix_even[7:0];
            want_g = ~pix_even[7:0];
            want_b = {pix_even[3:0], pix_even[7:4]};
            check_eq("probe_precrop_word", got[23:0],
                     {want_r, want_g, want_b});
        end
    end

    /* FULLRATE=0 converts the universal 188-sample default to 94 local
     * capture clocks, preserving the Denise-adapter framing.  Check the
     * reference before the full-width bank sweep advances the free-running
     * capture clock for thousands of AXI cycles. */
    line_seed = LINES - 1;
    legacy_buf_read(12'd4, got);
    sample_idx = (CROPH / 2) + 4 + CAPTURE_INPUT_OFFSET;
    pix_even = sample_idx / PIXSPAN + line_seed;
    want_r = pix_even[7:0];
    want_g = ~pix_even[7:0];
    want_b = {pix_even[3:0], pix_even[7:4]};
    check_eq("legacy_crop", got[23:0], {want_r, want_g, want_b});

    /* The final active raster line remains in its completed bank. */
    buf_rbank = FULLWIDTH ? cap_write_bank : 1'b0;
    for (k = (FULLWIDTH ? 0 : 4);
            k < (FULLWIDTH ? 1280 : 32); k = k + 1) begin
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

    /* A4000/A3000 video-slot capture runs at the full 28.37 MHz rate. Put
     * both PAL VSYNC phases after the crop origin so the legacy full-width
     * cap_x detector sees the true 908-clock half-line, then incorrectly
     * folds it through a 1024-count period to only 116. */
    drive_field_with_vsync_phase(100, 400);
    drive_field_with_vsync_phase(200, 400 + LINECLKS / 2);
    $display("A4000 phase probe x=%0d abs_delta=%0d changed=%0d",
             last_frame_sync_x, last_frame_phase_abs_delta,
             last_frame_phase_changed);
    check_eq("fullrate_halfline_interlace", cap_interlace, 1);
    interlace_field_parity = cap_y[0];
    drive_field_with_vsync_phase(300, 400);
    check_eq("fullrate_field_parity_b", cap_y[0],
             !interlace_field_parity);
    interlace_field_parity = cap_y[0];
    drive_field_with_vsync_phase(400, 400 + LINECLKS / 2);
    check_eq("fullrate_field_parity_a", cap_y[0],
             !interlace_field_parity);

    if (errors == 0)
        $display("RESULT PASS checks=%0d", checks);
    else
        $display("RESULT FAIL checks=%0d errors=%0d", checks, errors);
    $finish;
end

endmodule
