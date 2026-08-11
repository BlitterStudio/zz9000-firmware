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

/* Source-clock request engine.  The staged register bank and the three
 * operation-16 event sources live in mntzorro.v; this module owns the
 * pending and acknowledged state so it can also be exercised by xsim. */
module videocap_control_source #(
    parameter integer FULLRATE = 0
) (
    input  wire        source_clk,
    input  wire        request_event,
    input  wire [31:0] request_raw,
    input  wire        request_token_valid,
    input  wire        control_received,
    output reg         control_send = 0,
    output reg  [26:0] control_payload =
        {12'd26, 12'd188, 1'b0, 2'd0},
    output wire        busy,
    output reg  [7:0]  request_sequence = 0,
    output reg  [7:0]  applied_sequence = 0,
    output reg         last_commit_rejected = 0,
    output reg         applied_valid = 0,
    output reg  [31:0] applied_raw =
        {2'b00, 1'b0, 1'b0, 12'd26, 12'd188, 1'b0, 1'b0, 2'd0},
    output reg  [31:0] applied_effective_crop =
        {4'b0000, 12'd26, 4'b0000, 12'd188}
);

localparam [1:0] CONTROL_IDLE   = 2'd0;
localparam [1:0] CONTROL_LOAD   = 2'd1;
localparam [1:0] CONTROL_SEND   = 2'd2;
localparam [1:0] CONTROL_RETURN = 2'd3;

localparam [11:0] CROP_H_COMPAT = 12'd188;
localparam [11:0] CROP_V_COMPAT = 12'd26;
localparam [11:0] CROP_H_FULLRATE = 12'd279;
localparam [11:0] CROP_V_FULLRATE = 12'd40;

reg [1:0] control_state = CONTROL_IDLE;
reg [31:0] pending_raw =
    {2'b00, 1'b0, 1'b0, 12'd26, 12'd188, 1'b0, 1'b0, 2'd0};

wire request_fullrate_path = (FULLRATE != 0) && request_raw[2];
wire [11:0] request_crop_h_effective = request_raw[28] ?
    (request_fullrate_path ? CROP_H_FULLRATE : CROP_H_COMPAT) :
    request_raw[15:4];
wire [11:0] request_crop_v_effective = request_raw[29] ?
    (request_fullrate_path ? CROP_V_FULLRATE : CROP_V_COMPAT) :
    request_raw[27:16];
wire request_raw_valid =
    request_raw[31:30] == 2'b00 && request_raw[1:0] <= 2'd2;

assign busy = (control_state != CONTROL_IDLE);

always @(posedge source_clk) begin
    /* An event observed anywhere in the four-phase busy interval is rejected
     * without replacing the XPM-held payload or changing either sequence. */
    if (request_event && control_state != CONTROL_IDLE)
        last_commit_rejected <= 1'b1;

    case (control_state)
        CONTROL_IDLE: begin
            if (request_event) begin
                if (request_token_valid && request_raw_valid) begin
                    pending_raw <= request_raw;
                    control_payload <= {request_crop_v_effective,
                                        request_crop_h_effective,
                                        request_raw[2], request_raw[1:0]};
                    request_sequence <= request_sequence + 1'b1;
                    last_commit_rejected <= 1'b0;
                    control_state <= CONTROL_LOAD;
                end else begin
                    last_commit_rejected <= 1'b1;
                end
            end
        end

        /* LOAD gives the stable payload a complete source clock before SEND. */
        CONTROL_LOAD: begin
            control_send <= 1'b1;
            control_state <= CONTROL_SEND;
        end

        CONTROL_SEND: begin
            if (control_received) begin
                applied_raw <= pending_raw;
                applied_effective_crop <= {
                    4'b0000, control_payload[26:15],
                    4'b0000, control_payload[14:3]
                };
                applied_sequence <= request_sequence;
                applied_valid <= 1'b1;
                control_send <= 1'b0;
                control_state <= CONTROL_RETURN;
            end
        end

        CONTROL_RETURN: begin
            if (!control_received)
                control_state <= CONTROL_IDLE;
        end
    endcase
end

endmodule

/* Publish invalid/PAL/NTSC as one encoded value.  The candidate must agree
 * for two complete frames, and every validity change crosses coherently. */
module videocap_standard_cdc (
    input  wire       cap_clk,
    input  wire       axi_clk,
    input  wire       frame_complete,
    input  wire       frame_ntsc,
    output reg  [1:0] standard_axi = 0
);

localparam [1:0] STANDARD_INVALID = 2'd0;
localparam [1:0] STANDARD_PAL = 2'd1;
localparam [1:0] STANDARD_NTSC = 2'd2;

localparam [1:0] STANDARD_IDLE = 2'd0;
localparam [1:0] STANDARD_LOAD = 2'd1;
localparam [1:0] STANDARD_SEND = 2'd2;
localparam [1:0] STANDARD_RETURN = 2'd3;

reg candidate_valid = 0;
reg candidate_ntsc = 0;
reg [1:0] standard_cap = STANDARD_INVALID;
reg [1:0] standard_sent = STANDARD_INVALID;
reg [1:0] standard_payload = STANDARD_INVALID;
reg [1:0] standard_state = STANDARD_IDLE;
reg standard_send = 0;
wire standard_received;
wire [1:0] standard_dest_payload;
wire standard_dest_req;

xpm_cdc_handshake #(
    .DEST_EXT_HSK(0),
    .DEST_SYNC_FF(4),
    .INIT_SYNC_FF(1),
    .SIM_ASSERT_CHK(0),
    .SRC_SYNC_FF(4),
    .WIDTH(2)
) videocap_standard_handshake (
    .src_clk(cap_clk),
    .src_in(standard_payload),
    .src_send(standard_send),
    .src_rcv(standard_received),
    .dest_clk(axi_clk),
    .dest_out(standard_dest_payload),
    .dest_req(standard_dest_req),
    .dest_ack(1'b0)
);

always @(posedge cap_clk) begin
    if (frame_complete) begin
        if (!candidate_valid) begin
            candidate_valid <= 1'b1;
            candidate_ntsc <= frame_ntsc;
            standard_cap <= STANDARD_INVALID;
        end else if (candidate_ntsc != frame_ntsc) begin
            candidate_ntsc <= frame_ntsc;
            standard_cap <= STANDARD_INVALID;
        end else begin
            standard_cap <= frame_ntsc ? STANDARD_NTSC : STANDARD_PAL;
        end
    end

    case (standard_state)
        STANDARD_IDLE: begin
            if (standard_cap != standard_sent) begin
                standard_payload <= standard_cap;
                standard_state <= STANDARD_LOAD;
            end
        end
        STANDARD_LOAD: begin
            standard_send <= 1'b1;
            standard_state <= STANDARD_SEND;
        end
        STANDARD_SEND: begin
            if (standard_received) begin
                standard_send <= 1'b0;
                standard_sent <= standard_payload;
                standard_state <= STANDARD_RETURN;
            end
        end
        STANDARD_RETURN: begin
            if (!standard_received)
                standard_state <= STANDARD_IDLE;
        end
    endcase
end

always @(posedge axi_clk) begin
    if (standard_dest_req)
        standard_axi <= standard_dest_payload;
end

endmodule

module videocap_sampler #(
    parameter integer BUF_DEPTH   = 2048,
    parameter integer RGB_MODE    = 0,
    parameter integer CSYNC_VSYNC = 0,
    parameter integer FULLRATE    = 0,
    parameter integer PROBE_LINE = 120,
    parameter integer PROBE_SOURCE_X = 928
) (
    input  wire        cap_clk,
    input  wire        vcap_vsync,
    input  wire        vcap_hsync,
    input  wire [7:0]  vcap_r,
    input  wire [7:0]  vcap_g,
    input  wire [7:0]  vcap_b,

    input  wire        ctl_send,
    input  wire [26:0] ctl_payload,
    output wire        ctl_received,
    input  wire        ctl_read_full_width,
    output wire [1:0]  detected_standard,

    output reg  [10:0] cap_x,
    output reg  [10:0] cap_y,
    output reg  [10:0] cap_ymax,
    output reg         cap_interlace,
    output reg         cap_ntsc,
    output reg         cap_x_done,
    output reg         cap_shres,
    output reg         cap_line_toggle = 0,
    output wire        cap_write_bank,

    input  wire        probe_arm_toggle,
    output reg         probe_arm_seen = 0,
    output reg         probe_valid = 0,
    output reg  [511:0] probe_data = 0,
    output reg  [9:0]  probe_line = 0,
    output reg  [11:0] probe_source_x = 0,
    output reg  [31:0] probe_context = 0,
    output reg  [31:0] probe_config = 0,
    output reg         probe_precrop_valid = 0,
    output reg  [31:0] probe_precrop_context = 0,
    input  wire [5:0]  probe_precrop_raddr,
    output wire [31:0] probe_precrop_rdata,

    input  wire        axi_clk,
    input  wire        buf_rbank,
    input  wire [11:0] buf_raddr,
    output wire [31:0] buf_rdata
);

/* The source holds this bundled payload for the complete four-phase XPM
 * transaction.  External destination acknowledgement delays completion
 * until the next capture frame boundary. */
wire [26:0] ctl_dest_payload;
wire ctl_dest_req;
reg ctl_dest_ack = 0;

xpm_cdc_handshake #(
    .DEST_EXT_HSK(1),
    .DEST_SYNC_FF(4),
    .INIT_SYNC_FF(1),
    .SIM_ASSERT_CHK(0),
    .SRC_SYNC_FF(4),
    .WIDTH(27)
) videocap_control_handshake (
    .src_clk(axi_clk),
    .src_in(ctl_payload),
    .src_send(ctl_send),
    .src_rcv(ctl_received),
    .dest_clk(cap_clk),
    .dest_out(ctl_dest_payload),
    .dest_req(ctl_dest_req),
    .dest_ack(ctl_dest_ack)
);

reg [1:0] ctl_sample_mode_cap = 2'd0;
reg ctl_full_width_cap = 1'b0;
reg [11:0] ctl_crop_h_cap = 12'd188;
reg [11:0] ctl_crop_v_cap = 12'd26;

reg [6:0] hs = 0;
reg [6:0] vs = 0;
reg [23:0] rgbin = 0;
reg [10:0] sample_x = 0;
reg [10:0] raw_y = 0;
reg lace_field = 0;
reg next_lace_field = 0;
reg [3:0] shortlines = 0;
reg [7:0] hs_pulse_width = 0;
reg [11:0] phase_x = 0;
reg [11:0] phase_line_period = 0;
reg [11:0] vsync_phase_x = 0;

localparam integer LINEBUF_BANKS = (FULLRATE != 0) ? 2 : 1;
reg [31:0] linebuf [0:(BUF_DEPTH * LINEBUF_BANKS)-1];
reg [31:0] buf_rdata_r;
assign buf_rdata = buf_rdata_r;

reg capture_bank = 0;
wire capture_banking_cap = (FULLRATE != 0) && ctl_full_width_cap;
wire read_banking_axi = (FULLRATE != 0) && ctl_read_full_width;
wire [11:0] capture_buf_addr = {
    capture_banking_cap ? capture_bank : 1'b0, cap_x
};
wire [11:0] read_buf_addr = {
    read_banking_axi ? buf_rbank : 1'b0, buf_raddr[10:0]
};
assign cap_write_bank = capture_banking_cap ? capture_bank : 1'b0;

always @(posedge axi_clk)
    buf_rdata_r <= linebuf[read_buf_addr];

/* Detect the half-line phase change in capture-clock units, independently
 * of crop/filter/full-width pixel storage.  cap_x is not a horizontal phase
 * counter: it begins at the crop origin and advances at either one or one
 * half of the capture clock.  Folding its 11-bit full-width value through
 * the old 1024-count modulus reduced a real 908-clock PAL half-line to 116,
 * below the interlace threshold.  Measure the raw line period so a stable
 * VSYNC edge straddling HSYNC still has a small circular distance. */
localparam [11:0] INTERLACE_PHASE_DELTA = 12'h080;
wire [11:0] vsync_phase_abs_delta =
    (phase_x > vsync_phase_x) ?
    (phase_x - vsync_phase_x) : (vsync_phase_x - phase_x);
/* Both directions around the measured line must exceed the threshold.
 * This is equivalent to min(abs_delta, period - abs_delta) >= threshold,
 * without putting a second subtract-and-min chain on cap_interlace. */
wire [12:0] vsync_phase_abs_plus_threshold =
    {1'b0, vsync_phase_abs_delta} + {1'b0, INTERLACE_PHASE_DELTA};
wire vsync_phase_changed =
    (vsync_phase_abs_delta >= INTERLACE_PHASE_DELTA) &&
    (vsync_phase_abs_plus_threshold <= {1'b0, phase_line_period});

wire frame_sync = (CSYNC_VSYNC != 0) ?
    (hs[6:1] == 6'b000111 && hs_pulse_width >= 8'd128) :
    (vs[6:1] == 6'b111000);
wire line_sync = (hs[6:1] == 6'b000111);
wire completed_frame_ntsc = (raw_y >= 11'h190) ?
    ((raw_y >= 11'h23a) ? 1'b0 : 1'b1) :
    ((raw_y >= ((CSYNC_VSYNC != 0) ? 11'h130 : 11'h138)) ?
        1'b0 : 1'b1);

videocap_standard_cdc videocap_standard_publish (
    .cap_clk(cap_clk),
    .axi_clk(axi_clk),
    .frame_complete(frame_sync && raw_y != 0),
    .frame_ntsc(completed_frame_ntsc),
    .standard_axi(detected_standard)
);

/*
 * The control interface always expresses crop_h in 28 MHz samples. Denise
 * adapters retain the 14 MHz front end, so one local capture clock consumes
 * two control units there. This keeps the universal default (188) equivalent
 * to the historical 94-clock crop without requiring a firmware variant.
 */
wire [11:0] crop_h_local = (FULLRATE != 0) ?
    ctl_crop_h_cap : {1'b0, ctl_crop_h_cap[11:1]};
wire [11:0] probe_precrop_start = crop_h_local - 12'd64;

reg half = 0;
reg [23:0] rgb_prev = 0;
wire filter_pairs = (FULLRATE != 0) && !ctl_full_width_cap;

/* SuperHires changes within a 28 MHz sample pair; hires and lores do not.
 * Keep classification independent of whether that pair is stored separately
 * or filtered into one output pixel. */
reg shres_half = 0;
reg [23:0] shres_prev = 0;

wire [8:0] avg_r_sum = {1'b0, rgb_prev[23:16]} +
                       {1'b0, rgbin[23:16]} + 9'd1;
wire [8:0] avg_g_sum = {1'b0, rgb_prev[15:8]} +
                       {1'b0, rgbin[15:8]} + 9'd1;
wire [8:0] avg_b_sum = {1'b0, rgb_prev[7:0]} +
                       {1'b0, rgbin[7:0]} + 9'd1;
wire [23:0] rgb_average = {avg_r_sum[8:1], avg_g_sum[8:1],
                           avg_b_sum[8:1]};
wire [23:0] filtered_sample =
    (ctl_sample_mode_cap == 2'd1) ? rgb_prev :
    (ctl_sample_mode_cap == 2'd2) ? rgbin : rgb_average;
wire [31:0] capture_store_word = {8'b0,
    filter_pairs ? filtered_sample : rgbin};

/* Full-width crop_h names the first displayed 28 MHz sample, so preserve its
 * complete 1280-sample window.  The filtered/legacy path retains its
 * historical three-pixel settling guard. */
wire capture_head_valid = capture_banking_cap ?
    1'b1 : (cap_x > 11'd2);

wire probe_arm_toggle_cap;
reg probe_waiting = 0;
reg probe_publish_pending = 0;
reg [15:0] probe_seen_mask = 0;
reg probe_precrop_waiting = 0;
reg probe_precrop_publish_pending = 0;
reg [31:0] probe_precrop_mem [0:63];
assign probe_precrop_rdata = probe_precrop_mem[probe_precrop_raddr];

xpm_cdc_single #(
    .DEST_SYNC_FF(3),
    .INIT_SYNC_FF(1),
    .SIM_ASSERT_CHK(0),
    .SRC_INPUT_REG(0)
) videocap_probe_arm_cdc (
    .src_clk(axi_clk),
    .src_in(probe_arm_toggle),
    .dest_clk(cap_clk),
    .dest_out(probe_arm_toggle_cap)
);

reg [15:0] diff_count = 0;

always @(posedge cap_clk) begin
    if (!ctl_dest_req)
        ctl_dest_ack <= 1'b0;
    else if (!ctl_dest_ack && frame_sync) begin
        ctl_sample_mode_cap <= ctl_dest_payload[1:0];
        ctl_full_width_cap <= ctl_dest_payload[2];
        ctl_crop_h_cap <= ctl_dest_payload[14:3];
        ctl_crop_v_cap <= ctl_dest_payload[26:15];
        ctl_dest_ack <= 1'b1;
    end

    if (line_sync) begin
        if (phase_x != 0)
            phase_line_period <= phase_x;
        phase_x <= 0;
    end else if (phase_x != 12'hfff) begin
        phase_x <= phase_x + 1'b1;
    end

    if (probe_arm_seen != probe_arm_toggle_cap) begin
        probe_arm_seen <= probe_arm_toggle_cap;
        probe_valid <= 0;
        probe_waiting <= 1;
        probe_publish_pending <= 0;
        probe_seen_mask <= 0;
        probe_precrop_valid <= 0;
        probe_precrop_waiting <= 1;
        probe_precrop_publish_pending <= 0;
    end else if (probe_publish_pending) begin
        /* The complete 512-bit snapshot has been stable for one capture
         * clock before valid crosses back to AXI. */
        probe_valid <= 1;
        probe_waiting <= 0;
        probe_publish_pending <= 0;
    end

    if (probe_arm_seen == probe_arm_toggle_cap &&
            probe_precrop_publish_pending) begin
        /* As with the primary sampler snapshot, hold all pre-crop words stable
         * for one capture clock before valid crosses into the AXI domain. */
        probe_precrop_valid <= 1;
        probe_precrop_waiting <= 0;
        probe_precrop_publish_pending <= 0;
    end

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
        cap_x_done <= 0;
        if (cap_ymax >= 11'h190)
            cap_interlace <= 0;
        else if (CSYNC_VSYNC != 0)
            cap_interlace <= (next_lace_field != lace_field);
        else
            cap_interlace <=
                vsync_phase_changed;

        if (CSYNC_VSYNC != 0)
            lace_field <= next_lace_field;
        else begin
            vsync_phase_x <= phase_x;
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
        shres_half <= 0;
        cap_x_done <= 0;
        if (capture_banking_cap)
            capture_bank <= ~capture_bank;

        if (CSYNC_VSYNC != 0) begin
            if (hs_pulse_width < 8'h20) begin
                shortlines <= shortlines + 1'b1;
                if (shortlines == 0)
                    next_lace_field <= (cap_x >= 11'h200);
            end else begin
                shortlines <= 0;
            end
        end

        if (raw_y > ctl_crop_v_cap[10:0]) begin
            if (cap_interlace)
                cap_y <= cap_y + 2'b10;
            else
                cap_y <= cap_y + 1'b1;
        end
        raw_y <= raw_y + 1'b1;
    end else begin
        sample_x <= sample_x + 1'b1;

        /* Snapshot the 64 raw RGB samples immediately before the configured
         * crop origin.  The preceding post-window probe found only blanking,
         * so this isolates the other side of the selected 1280-sample window
         * without changing the capture or display path. */
        if (capture_banking_cap && crop_h_local >= 12'd64 &&
                probe_precrop_waiting &&
                !probe_precrop_publish_pending && cap_y == PROBE_LINE &&
                {1'b0, sample_x} >= probe_precrop_start &&
                {1'b0, sample_x} < crop_h_local) begin
            probe_precrop_mem[{1'b0, sample_x} - probe_precrop_start]
                <= {8'b0, rgbin};

            if ({1'b0, sample_x} == probe_precrop_start)
                probe_precrop_context <= {9'h000, capture_bank,
                                           raw_y, sample_x};
            if ({1'b0, sample_x} == crop_h_local - 1'b1)
                probe_precrop_publish_pending <= 1;
        end

        /* Keep the SHR pair phase tied to line sync, not to the runtime crop
         * origin.  An odd crop value must not re-pair adjacent hires pixels
         * and falsely classify them as SuperHires.  The first pair crossing
         * the crop boundary is ignored because one sample lies outside the
         * captured window. */
        if (FULLRATE != 0) begin
            if (!shres_half) begin
                shres_prev <= rgbin;
                shres_half <= 1;
            end else begin
                shres_half <= 0;
                if ({1'b0, sample_x} > crop_h_local &&
                        cap_x < (ctl_full_width_cap ? 11'h500 : 11'h200) &&
                        (rgbin !== shres_prev) && diff_count != 16'hffff)
                    diff_count <= diff_count + 1'b1;
            end
        end

        if ({1'b0, sample_x} < crop_h_local) begin
            half <= 0;
        end else begin
            if (filter_pairs) begin
                if (!half) begin
                    rgb_prev <= rgbin;
                    half <= 1;
                end else begin
                    half <= 0;
                    if (capture_head_valid)
                        linebuf[capture_buf_addr] <= {8'b0, filtered_sample};
                    else
                        linebuf[capture_buf_addr] <= 32'b0;
                    cap_x <= cap_x + 1'b1;
                end
            end else begin
                if (capture_head_valid)
                    linebuf[capture_buf_addr] <= {8'b0, rgbin};
                else
                    linebuf[capture_buf_addr] <= 32'b0;
                cap_x <= cap_x + 1'b1;
            end

            /* Snapshot the exact word presented to the sampler line-buffer
             * write port. AXI probing is held off until this source burst is
             * complete, so both snapshots describe the same captured row. */
            if (capture_banking_cap && probe_waiting &&
                    !probe_publish_pending && cap_y == PROBE_LINE &&
                    cap_x >= PROBE_SOURCE_X &&
                    cap_x < PROBE_SOURCE_X + 16) begin
                if (cap_x == PROBE_SOURCE_X) begin
                    probe_seen_mask <= 16'h0001;
                    probe_line <= cap_y[9:0];
                    probe_source_x <= cap_x;
                    probe_context <= {9'h000, capture_bank,
                                      raw_y, sample_x};
                    probe_config <= {7'h00, ctl_full_width_cap,
                                     ctl_crop_v_cap, ctl_crop_h_cap};
                end else begin
                    probe_seen_mask[cap_x - PROBE_SOURCE_X] <= 1'b1;
                end

                case (cap_x)
                    PROBE_SOURCE_X + 0:
                        probe_data[31:0] <= capture_store_word;
                    PROBE_SOURCE_X + 1:
                        probe_data[63:32] <= capture_store_word;
                    PROBE_SOURCE_X + 2:
                        probe_data[95:64] <= capture_store_word;
                    PROBE_SOURCE_X + 3:
                        probe_data[127:96] <= capture_store_word;
                    PROBE_SOURCE_X + 4:
                        probe_data[159:128] <= capture_store_word;
                    PROBE_SOURCE_X + 5:
                        probe_data[191:160] <= capture_store_word;
                    PROBE_SOURCE_X + 6:
                        probe_data[223:192] <= capture_store_word;
                    PROBE_SOURCE_X + 7:
                        probe_data[255:224] <= capture_store_word;
                    PROBE_SOURCE_X + 8:
                        probe_data[287:256] <= capture_store_word;
                    PROBE_SOURCE_X + 9:
                        probe_data[319:288] <= capture_store_word;
                    PROBE_SOURCE_X + 10:
                        probe_data[351:320] <= capture_store_word;
                    PROBE_SOURCE_X + 11:
                        probe_data[383:352] <= capture_store_word;
                    PROBE_SOURCE_X + 12:
                        probe_data[415:384] <= capture_store_word;
                    PROBE_SOURCE_X + 13:
                        probe_data[447:416] <= capture_store_word;
                    PROBE_SOURCE_X + 14:
                        probe_data[479:448] <= capture_store_word;
                    PROBE_SOURCE_X + 15:
                        probe_data[511:480] <= capture_store_word;
                endcase

                if (cap_x == PROBE_SOURCE_X + 15 &&
                        probe_seen_mask[14:0] == 15'h7fff)
                    probe_publish_pending <= 1;
            end

        end

        if (capture_banking_cap) begin
            if (!cap_x_done && cap_x >= 11'd1279) begin
                cap_x_done <= 1;
                cap_line_toggle <= ~cap_line_toggle;
            end
        end else begin
            cap_x_done <= (cap_x > 11'h200);
        end
    end
end

endmodule
