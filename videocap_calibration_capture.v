`timescale 1ns/1ps
/*
 * Frozen raw-pixel ROI for native-video phase calibration.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Toggle arm_toggle once per request and wait for status[2] to acknowledge
 * it. status[0] then qualifies all 1024 words, metadata and geometry. The
 * capture starts at the next field boundary and remains frozen until rearm.
 * A new arm invalidates the AXI result immediately, before the request CDC.
 * Reset aborts the request; after recovery software must toggle arm again.
 * Missing frame boundaries leave the request busy; software must bound its
 * wait. A boundary interrupting a partial ROI ends that request invalid/idle.
 *
 * Coordinates and RGB refer to the same pre-filter cap_clk sample. Crop is
 * in these local raw-coordinate units. Memory is 256 consecutive columns
 * beginning at crop_h+128 on source lines crop_v+64 through crop_v+67.
 */
module videocap_calibration_capture (
    input  wire        cap_clk,
    input  wire        cap_reset,
    input  wire        frame_sync,
    input  wire [10:0] raw_x,
    input  wire [10:0] raw_y,
    input  wire [11:0] crop_h,
    input  wire [11:0] crop_v,
    input  wire [23:0] rgb,
    input  wire        interlace,
    input  wire        field_parity,
    input  wire        ntsc,
    input  wire        axi_clk,
    input  wire        axi_resetn,
    input  wire        arm_toggle,
    input  wire [9:0]  read_addr,
    output reg  [31:0] read_data,
    output reg  [31:0] status,
    output reg  [31:0] geometry
);

localparam [2:0] IDLE = 3'd0;
localparam [2:0] WAIT_FRAME = 3'd1;
localparam [2:0] LATCH_METADATA = 3'd2;
localparam [2:0] CAPTURE = 3'd3;
localparam [2:0] PUBLISH = 3'd4;
localparam [2:0] FROZEN = 3'd5;

(* ram_style = "block" *) reg [23:0] pixels [0:1023];
(* ASYNC_REG = "TRUE" *) reg [2:0] arm_sync;
(* ASYNC_REG = "TRUE" *) reg [2:0] reset_cap;
reg [2:0] startup;
reg cap_ready;
reg arm_seen;
reg cap_valid;
reg cap_busy;
reg [2:0] state;
reg [15:0] field_sequence;
reg [15:0] snapshot_sequence;
reg [2:0] snapshot_mode;
reg [23:0] snapshot_geometry;
reg [11:0] expected_x;
reg [11:0] expected_y;
reg [9:0] write_addr;

wire [11:0] roi_x_first = snapshot_geometry[11:0] + 12'd128;
wire [11:0] roi_x_last = snapshot_geometry[11:0] + 12'd383;
wire [11:0] roi_y_first = snapshot_geometry[23:12] + 12'd64;
wire [11:0] roi_y_last = snapshot_geometry[23:12] + 12'd67;
wire [11:0] sample_x = {1'b0, raw_x};
wire [11:0] sample_y = {1'b0, raw_y};
wire in_roi = sample_x >= roi_x_first && sample_x <= roi_x_last &&
              sample_y >= roi_y_first && sample_y <= roi_y_last;
wire geometry_changed = {crop_v, crop_h} != snapshot_geometry;
wire mode_changed = {ntsc, field_parity, interlace} != snapshot_mode;
wire new_arm = arm_sync[2] != arm_seen;
wire sample_matches = sample_x == expected_x && sample_y == expected_y;
wire reset_request = cap_reset || !axi_resetn;

/* AXI reset can release independently of cap_clk. Synchronize the release
 * of either reset before any transaction state is allowed to advance. */
always @(posedge cap_clk or posedge reset_request) begin
    if (reset_request)
        reset_cap <= 3'b111;
    else
        reset_cap <= {reset_cap[1:0], 1'b0};
end

/* Keep reset outside the inferred dual-clock RAM. A read is meaningful
 * only with valid set; an aborted capture can leave partial words behind. */
always @(posedge cap_clk) begin
    if (!reset_cap[2] && cap_ready && !new_arm &&
            state == CAPTURE && !frame_sync && !geometry_changed &&
            !mode_changed && in_roi && sample_matches)
        pixels[write_addr] <= rgb;
end

always @(posedge axi_clk) begin
    if (!axi_resetn)
        read_data <= 0;
    else
        read_data <= {8'b0, pixels[read_addr]};
end

/* cap_reset is supplied with asynchronous assertion and capture-clock
 * synchronous release. Assertion also works when the input clock stops.
 * AXI reset aborts both sides of the transaction. */
always @(posedge cap_clk or posedge reset_cap[2]) begin
    if (reset_cap[2]) begin
        arm_sync <= 0;
        startup <= 0;
        cap_ready <= 0;
        arm_seen <= 0;
        cap_valid <= 0;
        cap_busy <= 0;
        state <= IDLE;
        field_sequence <= 0;
        snapshot_sequence <= 0;
        snapshot_mode <= 0;
        snapshot_geometry <= 0;
        expected_x <= 0;
        expected_y <= 0;
        write_addr <= 0;
    end else begin
        arm_sync <= {arm_sync[1:0], arm_toggle};
        if (frame_sync)
            field_sequence <= field_sequence + 1'b1;

        if (!cap_ready) begin
            startup <= {startup[1:0], 1'b1};
            if (&startup) begin
                /* Baseline the settled level, rather than interpreting
                 * a pre-reset high toggle as a fresh request. */
                arm_seen <= arm_sync[2];
                cap_ready <= 1;
            end
        end else if (new_arm) begin
            arm_seen <= arm_sync[2];
            cap_valid <= 0;
            cap_busy <= 1;
            write_addr <= 0;
            state <= WAIT_FRAME;
        end else begin
            case (state)
                WAIT_FRAME: begin
                    if (frame_sync)
                        state <= LATCH_METADATA;
                end
                LATCH_METADATA: begin
                    /* Sampler parity, standard and applied crop can all
                     * change on the frame_sync edge. Latch one clock later
                     * so this metadata describes the incoming field. */
                    if (!frame_sync) begin
                        snapshot_sequence <= field_sequence;
                        snapshot_mode <= {ntsc, field_parity, interlace};
                        snapshot_geometry <= {crop_v, crop_h};
                        expected_x <= crop_h + 12'd128;
                        expected_y <= crop_v + 12'd64;
                        write_addr <= 0;
                        /* Reject an ROI beyond the 11-bit source raster;
                         * no truncation or wrap may produce a false pass. */
                        if (crop_h > 12'd1664 || crop_v > 12'd1980) begin
                            cap_busy <= 0;
                            state <= IDLE;
                        end else begin
                            state <= CAPTURE;
                        end
                    end
                end
                CAPTURE: begin
                    if (geometry_changed || mode_changed) begin
                        /* A mid-field transition restarts only at a clean
                         * subsequent boundary; never mix configurations. */
                        write_addr <= 0;
                        state <= WAIT_FRAME;
                    end else if (frame_sync) begin
                        /* The field ended before all coordinates arrived.
                         * End invalid; software bounds a missing-field wait. */
                        cap_busy <= 0;
                        state <= IDLE;
                    end else if (in_roi) begin
                        if (!sample_matches) begin
                            /* Missing, repeated or reordered ROI pixels are
                             * incomplete capture, even if the total is 1024. */
                            cap_busy <= 0;
                            state <= IDLE;
                        end else if (write_addr == 10'd1023) begin
                            state <= PUBLISH;
                        end else begin
                            write_addr <= write_addr + 1'b1;
                            if (expected_x == roi_x_last) begin
                                expected_x <= roi_x_first;
                                expected_y <= expected_y + 1'b1;
                            end else begin
                                expected_x <= expected_x + 1'b1;
                            end
                        end
                    end
                end
                PUBLISH: begin
                    /* The final RAM write and bundled metadata have settled
                     * before valid starts its three-register AXI crossing. */
                    cap_valid <= 1;
                    state <= FROZEN;
                end
                FROZEN: begin
                    /* Let valid rise before busy drops, avoiding a false
                     * failure indication while completion crosses domains. */
                    cap_busy <= 0;
                end
                default: begin
                    cap_busy <= 0;
                end
            endcase
        end
    end
end

/* Single-bit controls cross through three registers. Bundled metadata uses
 * two registers and is frozen before valid; the destination cannot publish
 * it until the slower valid crossing completes. The RAM remains unwritten
 * until another arm, which immediately invalidates the AXI result. */
(* ASYNC_REG = "TRUE" *) reg [3:0] control_meta;
(* ASYNC_REG = "TRUE" *) reg [3:0] control_sync;
(* ASYNC_REG = "TRUE" *) reg [3:0] control_settled;
(* ASYNC_REG = "TRUE" *) reg [18:0] metadata_meta;
(* ASYNC_REG = "TRUE" *) reg [18:0] metadata_sync;
(* ASYNC_REG = "TRUE" *) reg [23:0] geometry_meta;
(* ASYNC_REG = "TRUE" *) reg [23:0] geometry_sync;
(* ASYNC_REG = "TRUE" *) reg [2:0] reset_axi;
reg axi_initialized;
reg arm_local;
reg local_armed;
reg valid_low_seen;

wire seen_axi = control_settled[2];
wire busy_axi = control_settled[1];
wire valid_axi = control_settled[0];

always @(posedge axi_clk or posedge reset_request) begin
    if (reset_request)
        reset_axi <= 3'b111;
    else
        reset_axi <= {reset_axi[1:0], 1'b0};
end

always @(posedge axi_clk) begin
    if (!axi_resetn || reset_axi[2]) begin
        control_meta <= 0;
        control_sync <= 0;
        control_settled <= 0;
        metadata_meta <= 0;
        metadata_sync <= 0;
        geometry_meta <= 0;
        geometry_sync <= 0;
        axi_initialized <= 0;
        arm_local <= arm_toggle;
        local_armed <= 0;
        valid_low_seen <= 0;
        status <= 0;
        geometry <= 0;
    end else begin
        control_meta <= {cap_ready, arm_seen, cap_busy, cap_valid};
        control_sync <= control_meta;
        control_settled <= control_sync;
        metadata_meta <= {snapshot_sequence, snapshot_mode};
        metadata_sync <= metadata_meta;
        geometry_meta <= snapshot_geometry;
        geometry_sync <= geometry_meta;

        if (!control_settled[3]) begin
            axi_initialized <= 0;
            arm_local <= arm_toggle;
            local_armed <= 0;
            valid_low_seen <= 0;
            status <= 0;
        end else if (!axi_initialized) begin
            axi_initialized <= 1;
            arm_local <= arm_toggle;
            status <= {29'd0, seen_axi, 2'b00};
        end else if (arm_toggle != arm_local) begin
            arm_local <= arm_toggle;
            local_armed <= 1;
            valid_low_seen <= 0;
            status[2:0] <= {seen_axi, 1'b1, 1'b0};
        end else begin
            status[2] <= seen_axi;
            status[1] <= local_armed && (seen_axi != arm_local || busy_axi);
            status[0] <= 0;
            /* A matching acknowledgement alone is insufficient: require
             * valid low for this request before accepting any completion. */
            if (local_armed && seen_axi == arm_local && !valid_axi)
                valid_low_seen <= 1;
            if (local_armed && seen_axi == arm_local &&
                    valid_low_seen && valid_axi) begin
                status <= {metadata_sync[18:3], 10'd0,
                           metadata_sync[2:0], seen_axi, 1'b0, 1'b1};
                geometry <= {8'd0, geometry_sync};
            end
        end
    end
end

endmodule
