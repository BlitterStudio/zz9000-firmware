`timescale 1ns/1ps
/* Integration coverage for the real sampler, input RGB registers, control
 * handshake, recovery gate and frozen raw calibration memory.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
module videocap_recovery_tb;
    reg cap_clk = 0, axi_clk = 0;
    reg cap_clock_enabled = 1;
    real cap_half_period = 17.621;
    reg cap_reset = 1, axi_resetn = 0;
    reg hsync = 1, vsync = 1, grid_ref = 0;
    reg [23:0] pins = 0;
    reg cal_arm = 0;
    reg [9:0] cal_address = 0;
    reg [3:0] cal_metadata_address = 0;
    wire [31:0] cal_status, cal_data, cal_geometry;
    wire [31:0] cal_metadata_data;
    wire capture_ready, line_toggle, anchor_toggle;
    wire [10:0] cap_x, cap_y;
    wire cap_ntsc, cap_interlace;
    reg request_event = 0;
    reg [31:0] request_raw = 0;
    wire control_send, control_received, control_busy, applied_valid;
    wire [26:0] control_payload;
    wire [31:0] applied_raw, applied_effective;
    integer checks = 0, failures = 0, line_events = 0, anchor_events = 0;
    reg last_line = 0, last_anchor = 0;
    integer old_lines, old_anchors;
    reg [31:0] frozen_status, frozen_geometry;
    reg [31:0] frozen_metadata [0:11];

    always begin
        #(cap_half_period);
        if (cap_clock_enabled) cap_clk = ~cap_clk;
        else cap_clk = 0;
    end
    always #5.003 axi_clk = ~axi_clk;

    videocap_control_source #(.FULLRATE(1)) control (
        .source_clk(axi_clk), .request_event(request_event),
        .request_raw(request_raw), .request_token_valid(1'b1),
        .control_received(control_received), .control_send(control_send),
        .control_payload(control_payload), .busy(control_busy),
        .applied_valid(applied_valid), .applied_raw(applied_raw),
        .applied_effective_crop(applied_effective)
    );

    videocap_sampler #(.BUF_DEPTH(2048), .RGB_MODE(0),
        .CSYNC_VSYNC(0), .FULLRATE(1)) dut (
        .cap_clk(cap_clk), .cap_reset(cap_reset), .capture_ready(capture_ready),
        .vcap_vsync(vsync), .vcap_hsync(hsync), .vcap_r(pins[23:16]),
        .vcap_g(pins[15:8]), .vcap_b(pins[7:0]), .grid_ref(grid_ref),
        .ctl_send(control_send), .ctl_payload(control_payload),
        .ctl_received(control_received), .ctl_read_full_width(applied_raw[2]),
        .cap_x(cap_x), .cap_y(cap_y), .cap_ntsc(cap_ntsc),
        .cap_interlace(cap_interlace), .cap_line_toggle(line_toggle),
        .cap_frame_anchor_toggle(anchor_toggle),
        .probe_arm_toggle(1'b0), .probe_precrop_raddr(6'd0),
        .axi_clk(axi_clk), .axi_resetn(axi_resetn), .cal_arm(cal_arm),
        .cal_address(cal_address), .cal_status(cal_status), .cal_data(cal_data),
        .cal_metadata_address(cal_metadata_address),
        .cal_metadata_data(cal_metadata_data),
        .cal_geometry(cal_geometry), .buf_rbank(1'b0), .buf_raddr(12'd0)
    );

    task require;
        input condition;
        input [767:0] description;
        begin
            checks = checks + 1;
            if (condition !== 1'b1) begin
                failures = failures + 1;
                if (failures < 24)
                    $display("MISMATCH %0s at %0t status=%08x geometry=%08x",
                        description, $time, cal_status, cal_geometry);
            end
        end
    endtask

    /* Every pixel changes all channels and line/field identity matters.
     * This oracle is based on driven pins, not the sampler's rgbin signal. */
    function [23:0] pixel;
        input integer x, line_number, seed;
        reg [7:0] r, g, b;
        begin
            r = x * 197 + seed;
            g = (x * 53) ^ (line_number * 7) ^ (seed * 3);
            b = (x >> 3) ^ (line_number * 19) ^ (seed * 11);
            pixel = {r, g, b};
        end
    endfunction

    /* Each high-sync pixel is stable for half a clock before capture. The
     * six-sample HSYNC decode plus reset-free one-clock IOB RGB register
     * gives raw sample 0 the pin value at high-sync pixel 4. This explicit
     * four-pixel offset is independent of crop/filter/full-width selection. */
    localparam integer INPUT_OFFSET = 4;
    task drive_line;
        input integer line_number, seed;
        integer x;
        begin
            for (x = 0; x < 1816; x = x + 1) begin
                @(negedge cap_clk);
                hsync = x >= 67;
                pins = pixel(x - 67, line_number, seed);
                grid_ref = ((x % 4) == 0);
                @(posedge cap_clk);
            end
            #0.001;
        end
    endtask

    task drive_field;
        input integer lines, seed;
        integer y;
        begin
            /* Allow the old high VSYNC state to settle, even after reset. */
            vsync = 1;
            repeat (12) @(negedge cap_clk);
            vsync = 0;
            drive_line(0, seed);
            drive_line(1, seed);
            vsync = 1;
            for (y = 2; y < lines; y = y + 1)
                drive_line(y, seed);
        end
    endtask

    always @(posedge cap_clk) begin
        #0.001;
        if (line_toggle != last_line) begin
            require(capture_ready, "no completed-line event while capture is recovering");
            line_events = line_events + 1;
            last_line = line_toggle;
        end
        if (anchor_toggle != last_anchor) begin
            require(capture_ready, "no frame-anchor event while capture is recovering");
            anchor_events = anchor_events + 1;
            last_anchor = anchor_toggle;
        end
    end

    task request_config;
        input integer h, v;
        input full_width;
        input [1:0] sample_mode;
        begin
            @(negedge axi_clk);
            request_raw = (v << 16) | (h << 4) | (full_width << 2) | sample_mode;
            request_event = 1;
            @(negedge axi_clk);
            request_event = 0;
            repeat (20) @(posedge axi_clk);
            #0.001;
            require(control_busy, "configuration request remains pending until a field boundary");
        end
    endtask

    task check_config;
        input integer h, v;
        input full_width;
        input [1:0] sample_mode;
        integer tries;
        begin
            tries = 0;
            while (control_busy && tries < 200) begin
                @(posedge axi_clk);
                tries = tries + 1;
            end
            #0.001;
            require(!control_busy && applied_valid, "configuration handshake completes after recovery boundary");
            require(applied_raw == ((v << 16) | (h << 4) | (full_width << 2) | sample_mode),
                "acknowledged control retains crop, width and sampling mode");
            require(applied_effective == ((v << 16) | h), "effective crop readback survives recovery");
        end
    endtask

    task arm;
        integer tries;
        begin
            @(negedge axi_clk);
            cal_arm = ~cal_arm;
            @(posedge axi_clk); #0.001;
            require(cal_status[1:0] == 2'b10, "new arm invalidates frozen data immediately");
            tries = 0;
            while (cal_status[2] !== cal_arm && tries < 200) begin
                @(posedge axi_clk); #0.001;
                tries = tries + 1;
            end
            require(cal_status[2] === cal_arm, "arm crosses the actual sampler capture domain");
        end
    endtask

    task check_snapshot;
        input integer h, v, seed;
        input ntsc;
        input full_width;
        input [1:0] sample_mode;
        integer tries, index, row, word_index, x, y;
        reg [31:0] expected, identity, timing, context;
        reg [15:0] first_timestamp;
        begin
            tries = 0;
            while (!cal_status[0] && tries < 200) begin
                @(posedge axi_clk); #0.001;
                tries = tries + 1;
            end
            require(cal_status[2:0] == {cal_arm, 2'b01}, "snapshot completed with current arm acknowledgement");
            require(cal_status[31:16] != 0, "snapshot identifies a completed source field");
            require(cal_status[5:3] == {ntsc, 2'b00}, "progressive PAL/NTSC telemetry matches source");
            require(cal_geometry == ((v << 12) | h), "snapshot contains applied raw crop geometry");
            first_timestamp = 0;
            for (row = 0; row < 4; row = row + 1) begin
                word_index = row * 3;
                @(negedge axi_clk); cal_metadata_address = word_index;
                @(posedge axi_clk); #0.001; identity = cal_metadata_data;
                @(negedge axi_clk); cal_metadata_address = word_index + 1;
                @(posedge axi_clk); #0.001; timing = cal_metadata_data;
                @(negedge axi_clk); cal_metadata_address = word_index + 2;
                @(posedge axi_clk); #0.001; context = cal_metadata_data;
                require(identity[31], "row metadata has prior-line timing history");
                require(identity[30], "row metadata saw the external capture grid");
                require(identity[26:16] == v + 64 + row,
                    "row metadata identifies the exact captured raw source row");
                require(timing[15:0] == 16'd1816,
                    "row metadata preserves the accepted line-edge interval");
                require(context[31:20] == 12'd1815,
                    "row metadata preserves the pre-reset sample counter");
                require(context[19:8] == 12'd1815,
                    "row metadata preserves the pre-reset phase counter");
                require(context[6:0] == {full_width, sample_mode, 1'b1, 1'b0, 2'b00},
                    "row metadata preserves the active sampler configuration");
                if (row == 0)
                    first_timestamp = timing[31:16];
                else
                    require(timing[31:16] == first_timestamp + row * 1816,
                        "row timestamps remain consecutive in the free-running clock domain");
            end
            for (index = 0; index < 1024; index = index + 1) begin
                @(negedge axi_clk);
                cal_address = index;
                @(posedge axi_clk); #0.001;
                x = h + 128 + (index % 256) + INPUT_OFFSET;
                y = v + 64 + (index / 256) - 1;
                expected = {8'b0, pixel(x, y, seed)};
                if (cal_data !== expected && failures < 20)
                    $display("PIXEL index=%0d got=%08x want=%08x x=%0d y=%0d",
                        index, cal_data, expected, x, y);
                require(cal_data === expected, "frozen sample equals raw pin pattern before filtering");
            end
        end
    endtask

    initial begin
        #1000000000;
        $display("RESULT FAIL recovery: watchdog expired");
        $finish;
    end

    initial begin
        $display("CASE configuration and line events through two-field recovery");
        repeat (24) @(posedge cap_clk);
        @(negedge axi_clk); axi_resetn = 1;
        require(!capture_ready, "capture is unavailable throughout reset");
        request_config(279, 40, 1'b1, 2'd0);
        require(!applied_valid, "reset cannot acknowledge the pending new configuration");
        @(negedge cap_clk); cap_reset = 0;
        old_lines = line_events;
        old_anchors = anchor_events;
        drive_field(312, 10);
        require(!capture_ready, "first field boundary remains suppressed");
        require(line_events == old_lines && anchor_events == old_anchors,
            "first recovery field emits no line or anchor tokens");
        check_config(279, 40, 1'b1, 2'd0);
        drive_field(312, 20);
        require(capture_ready, "second field boundary permits clean capture again");
        require(line_events > old_lines && anchor_events > old_anchors,
            "line and anchor events resume after the recovery boundary");

        $display("CASE actual sampler RGB/geometry integration and frozen payload");
        arm();
        drive_field(312, 30);
        check_snapshot(279, 40, 30, 1'b0, 1'b1, 2'd0);
        frozen_status = cal_status;
        frozen_geometry = cal_geometry;
        for (old_lines = 0; old_lines < 12; old_lines = old_lines + 1) begin
            @(negedge axi_clk); cal_metadata_address = old_lines;
            @(posedge axi_clk); #0.001;
            frozen_metadata[old_lines] = cal_metadata_data;
        end
        drive_field(312, 40);
        require(cal_status == frozen_status && cal_geometry == frozen_geometry,
            "new native fields do not mutate published snapshot metadata");
        for (old_lines = 0; old_lines < 12; old_lines = old_lines + 1) begin
            @(negedge axi_clk); cal_metadata_address = old_lines;
            @(posedge axi_clk); #0.001;
            require(cal_metadata_data == frozen_metadata[old_lines],
                "new native fields do not mutate frozen row timing metadata");
        end
        check_snapshot(279, 40, 30, 1'b0, 1'b1, 2'd0);

        $display("CASE rearm and same-boundary crop/filter change");
        request_config(189, 26, 1'b0, 2'd1);
        arm();
        drive_field(312, 50);
        check_config(189, 26, 1'b0, 2'd1);
        require(cal_status[31:16] != frozen_status[31:16], "rearm selects a new field sequence");
        check_snapshot(189, 26, 50, 1'b0, 1'b0, 2'd1);

        $display("CASE stopped capture clock invalidation and pending config recovery");
        old_lines = line_events;
        old_anchors = anchor_events;
        @(negedge cap_clk); cap_clock_enabled = 0;
        #1; cap_reset = 1;
        repeat (100) @(posedge axi_clk); #0.001;
        require(!capture_ready, "clock loss drops readiness without a capture-clock edge");
        require(cal_status[1:0] == 0, "AXI invalidates frozen capture while source clock is stopped");
        require(line_events == old_lines && anchor_events == old_anchors,
            "stopped-clock reset emits no fake toggle events");
        request_config(281, 41, 1'b1, 2'd2);
        require(control_busy, "configuration remains available and waits while source clock is stopped");
        @(negedge axi_clk); cal_arm = ~cal_arm;
        repeat (40) @(posedge axi_clk);
        require(!cal_status[0], "an arm during recovery cannot expose old memory as valid");
        cap_half_period = 17.460;
        cap_clock_enabled = 1;
        repeat (12) @(posedge cap_clk);
        @(negedge cap_clk); cap_reset = 0;
        drive_field(262, 60);
        require(!capture_ready, "first restarted field remains suppressed");
        require(line_events == old_lines && anchor_events == old_anchors,
            "first restarted field emits no stale line or anchor tokens");
        check_config(281, 41, 1'b1, 2'd2);
        drive_field(262, 70);
        require(capture_ready, "second restarted field restores capture readiness");
        require(!cal_status[0], "recovery requires a fresh arm, never replays an old request");
        arm();
        drive_field(262, 80);
        check_snapshot(281, 41, 80, 1'b1, 1'b1, 2'd2);
        require(line_events > old_lines && anchor_events > old_anchors,
            "native line output resumes with the new configuration");

        if (failures)
            $display("RESULT FAIL recovery: %0d failures / %0d checks", failures, checks);
        else
            $display("RESULT PASS recovery: %0d checks", checks);
        $finish;
    end
endmodule
