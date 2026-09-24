`timescale 1ns/1ps
/*
 * Row-origin discriminator for the native-video calibration snapshot.
 *
 * Two deliberately independent faults exercise an alternating one-clock
 * decoded HSYNC displacement and an alternating whole-pixel RGB displacement.
 * Row metadata must identify only the HSYNC case through its line interval,
 * pre-reset counters and capture-grid phase.  The horizontal window must now
 * reject that HSYNC displacement while leaving real RGB movement observable.
 *
 * The synthetic NTSC field uses 1820-clock full lines and six 910-clock
 * vertical-blanking half-lines.  It is a digital discriminator, not an analog
 * metastability or temperature model.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
module videocap_row_timing_tb;
    reg cap_clk = 0, axi_clk = 0, reset = 1;
    reg hsync = 1, vsync = 1, grid_ref = 0;
    reg [23:0] rgb = 0;
    reg arm = 0, control_send = 0;
    reg [9:0] address = 0;
    reg [3:0] metadata_address = 0;
    wire control_received, capture_ready;
    wire [31:0] data, status, geometry, metadata_data;
    integer checks = 0, failures = 0, grid_cycle = 0;

    always #17.5 cap_clk = ~cap_clk;
    always #5 axi_clk = ~axi_clk;

    videocap_sampler #(.FULLRATE(1), .RGB_MODE(0), .CSYNC_VSYNC(0)) dut (
        .cap_clk(cap_clk), .cap_reset(reset), .capture_ready(capture_ready),
        .axi_clk(axi_clk), .axi_resetn(!reset),
        .vcap_hsync(hsync), .vcap_vsync(vsync),
        .vcap_r(rgb[23:16]), .vcap_g(rgb[15:8]), .vcap_b(rgb[7:0]),
        .grid_ref(grid_ref), .ctl_send(control_send),
        .ctl_received(control_received),
        .ctl_payload({12'd40, 12'd279, 1'b1, 2'd0}),
        .ctl_read_full_width(1'b1), .buf_raddr(12'd0), .buf_rbank(1'b0),
        .probe_arm_toggle(1'b0), .probe_precrop_raddr(6'd0),
        .cal_arm(arm), .cal_address(address), .cal_data(data),
        .cal_status(status), .cal_geometry(geometry),
        .cal_metadata_address(metadata_address),
        .cal_metadata_data(metadata_data)
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
                        description, $time, status, geometry);
            end
        end
    endtask

    function [23:0] pattern;
        input integer pen;
        reg [7:0] r, g, b;
        begin
            r = pen * 197 + 101;
            g = {r[4:0], r[7:5]} ^ 8'h5a;
            b = {r[2:0], r[7:3]} ^ 8'ha5;
            pattern = {r, g, b};
        end
    endfunction

    task drive_segment;
        input integer mode, line_number, clocks;
        integer x, hsync_skew, hsync_low, rgb_skew, pattern_offset;
        begin
            hsync_skew = ((mode == 1 || mode == 3) &&
                (line_number % 2) == 0) ? 1 : 0;
            rgb_skew = (mode == 2 && (line_number % 2) == 0) ? 1 :
                ((mode == 3 && (line_number % 2) == 0) ? -1 : 0);
            hsync_low = 67 + hsync_skew;
            pattern_offset = 94;
            /* Replay the accepted-edge timing from the v0.8 hardware packet.
             * The row metadata is frozen one source segment after this task's
             * line_number, so segments 103..106 produce raw rows 104..107.
             * A constant 1820-clock physical line plus the 130/129/130/130
             * low-width sequence yields accepted intervals 1821/1819/1821/
             * 1820 exactly, without moving the raster-locked RGB pattern. */
            if (mode == 5) begin
                hsync_low = ((line_number == 103) ||
                    (line_number == 105) || (line_number == 106)) ? 136 : 135;
                pattern_offset = 150;
            end
            for (x = 0; x < clocks; x = x + 1) begin
                @(negedge cap_clk);
                hsync = x >= hsync_low;
                grid_ref = ((grid_cycle % 4) == 0);
                grid_cycle = grid_cycle + 1;
                rgb = pattern(x - pattern_offset + rgb_skew);
                if (mode == 4 && line_number == 103 && x == 490)
                    rgb = rgb ^ 24'h000001;
            end
        end
    endtask

    task drive_field;
        input integer mode;
        integer line_number;
        begin
            /* A vertical edge followed by six equalizing/serration-sized
             * half-lines exercises real NTSC-like blanking cadence without
             * changing the four active ROI line periods under test. */
            vsync = 0;
            for (line_number = 0; line_number < 6; line_number = line_number + 1)
                drive_segment(mode, line_number, 910);
            vsync = 1;
            for (line_number = 6; line_number < 262; line_number = line_number + 1)
                drive_segment(mode, line_number, 1820);
            @(negedge cap_clk);
            hsync = 1;
        end
    endtask

    task arm_snapshot;
        integer tries;
        begin
            @(negedge axi_clk); arm = ~arm;
            tries = 0;
            while ((status[2] !== arm || status[0]) && tries < 200) begin
                @(posedge axi_clk); #0.001;
                tries = tries + 1;
            end
            require(status[2] === arm && !status[0],
                "arm acknowledgement invalidates the old snapshot");
        end
    endtask

    task save_snapshot;
        input integer mode;
        integer fd, index, tries;
        reg [255:0] filename;
        begin
            $display("PROGRESS arm mode=%0d at %0t", mode, $time);
            arm_snapshot();
            $display("PROGRESS drive mode=%0d at %0t", mode, $time);
            drive_field(mode);
            tries = 0;
            while (!status[0] && tries < 200) begin
                @(posedge axi_clk); #0.001;
                tries = tries + 1;
            end
            require(status[2:0] == {arm, 2'b01},
                "snapshot completes with the current arm token");
            require(status[5] && !status[4],
                "synthetic field retains NTSC standard and stable parity");
            require(geometry == 32'h00028117,
                "snapshot retains the requested crop geometry");
            case (mode)
                0: filename = "baseline.txt";
                1: filename = "alternating_hsync.txt";
                2: filename = "alternating_rgb.txt";
                3: filename = "compensated.txt";
                4: filename = "rgb_bit.txt";
                default: filename = "real_packet.txt";
            endcase
            fd = $fopen(filename, "w");
            for (index = 0; index < 1024; index = index + 1) begin
                @(negedge axi_clk); address = index;
                @(posedge axi_clk); #0.001;
                $fdisplay(fd, "P %06x", data[23:0]);
            end
            for (index = 0; index < 12; index = index + 1) begin
                @(negedge axi_clk); metadata_address = index;
                @(posedge axi_clk); #0.001;
                $fdisplay(fd, "M %08x", metadata_data);
            end
            $fclose(fd);
            $display("CASE row timing mode=%0d status=%08x geometry=%08x",
                mode, status, geometry);
        end
    endtask

    initial begin
        repeat (16) @(negedge cap_clk);
        reset = 0;
        @(negedge cap_clk); control_send = 1;
        repeat (12) @(negedge cap_clk);
        /* The destination deliberately applies controls only at a field
         * boundary, so drive that boundary concurrently with the handshake. */
        fork
            begin
                wait (control_received);
                @(negedge cap_clk); control_send = 0;
                wait (!control_received);
            end
            begin
                drive_field(0);
            end
        join
        $display("PROGRESS configured at %0t", $time);

        /* Two boundaries are suppressed by recovery; the third confirms the
         * configured, grid-locked capture path is live before measurements. */
        drive_field(0);
        drive_field(0);
        $display("PROGRESS recovered at %0t", $time);
        require(capture_ready, "capture recovers before discriminator cases");

        save_snapshot(0);
        save_snapshot(1);
        save_snapshot(2);
        save_snapshot(3);
        save_snapshot(4);
        /* Shift the synthetic reference by one tick so the packet's accepted
         * grid phases reproduce 0/3/0/0 rather than an equivalent rotation. */
        grid_cycle = grid_cycle - 1;
        save_snapshot(5);

        if (failures)
            $display("RESULT FAIL row timing: %0d failures / %0d checks",
                failures, checks);
        else
            $display("RESULT PASS row timing: %0d checks", checks);
        $finish;
    end

    initial begin
        #600000000;
        $display("RESULT FAIL row timing: watchdog expired");
        $finish;
    end
endmodule
