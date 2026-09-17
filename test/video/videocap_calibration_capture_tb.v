`timescale 1ns/1ps

// Exercise the real dual-clock capture RAM and transaction interface.
module videocap_calibration_capture_tb;
    reg cap_clk = 0;
    reg axi_clk = 0;
    reg cap_clock_enabled = 1;
    real cap_half_period = 17.621;
    real axi_half_period = 5.003;
    reg cap_reset = 1;
    reg axi_resetn = 0;
    reg frame_sync = 0;
    reg [10:0] raw_x = 0;
    reg [10:0] raw_y = 0;
    reg [11:0] crop_h = 20;
    reg [11:0] crop_v = 10;
    reg [23:0] rgb = 0;
    reg interlace = 0;
    reg field_parity = 0;
    reg ntsc = 0;
    reg arm_toggle = 0;
    reg [9:0] read_addr = 0;
    wire [31:0] read_data;
    wire [31:0] status;
    wire [31:0] geometry;
    integer failures = 0;
    integer checks = 0;
    integer observed_sequence = 0;

    always begin
        #(cap_half_period);
        if (cap_clock_enabled) cap_clk = ~cap_clk;
        else cap_clk = 0;
    end
    always #(axi_half_period) axi_clk = ~axi_clk;

    videocap_calibration_capture dut (
        .cap_clk(cap_clk), .cap_reset(cap_reset), .frame_sync(frame_sync),
        .raw_x(raw_x), .raw_y(raw_y), .crop_h(crop_h), .crop_v(crop_v),
        .rgb(rgb), .interlace(interlace), .field_parity(field_parity),
        .ntsc(ntsc), .axi_clk(axi_clk), .axi_resetn(axi_resetn),
        .arm_toggle(arm_toggle), .read_addr(read_addr), .read_data(read_data),
        .status(status), .geometry(geometry)
    );

    task require;
        input condition;
        input [511:0] description;
        begin
            checks = checks + 1;
            if (condition !== 1'b1) begin
                failures = failures + 1;
                if (failures < 20)
                    $display("MISMATCH: %0s at %0t status=%08x geometry=%08x",
                             description, $time, status, geometry);
            end
        end
    endtask

    function [23:0] pixel_value;
        input integer x, y, seed;
        reg [7:0] red, green, blue;
        begin
            red = x ^ seed;
            green = y ^ (seed * 3);
            blue = (x >> 8) ^ (y >> 3) ^ (seed * 7);
            pixel_value = {red, green, blue};
        end
    endfunction

    task settle;
        begin
            repeat (16) @(posedge cap_clk);
            repeat (24) @(posedge axi_clk);
            #0.001;
        end
    endtask

    task arm;
        integer tries;
        begin
            @(negedge axi_clk);
            arm_toggle = ~arm_toggle;
            @(posedge axi_clk); #0.001;
            require(status[1:0] == 2'b10, "arm immediately invalidates result");
            tries = 0;
            while (status[2] !== arm_toggle && tries < 100) begin
                @(posedge axi_clk); #0.001;
                tries = tries + 1;
            end
            require(status[2] === arm_toggle, "capture acknowledges current arm");
            require(status[1:0] == 2'b10, "acknowledged arm awaits frame boundary");
        end
    endtask

    task boundary;
        input integer h, v;
        input lace, parity, standard_ntsc;
        begin
            @(negedge cap_clk);
            frame_sync = 1;
            raw_x = 0;
            raw_y = 0;
            @(posedge cap_clk);
            // Model the sampler's nonblocking metadata update at frame_sync.
            crop_h <= h;
            crop_v <= v;
            interlace <= lace;
            field_parity <= parity;
            ntsc <= standard_ntsc;
            observed_sequence = observed_sequence + 1;
            @(negedge cap_clk);
            frame_sync = 0;
            @(negedge cap_clk);
        end
    endtask

    task sample;
        input integer x, y, seed;
        begin
            @(negedge cap_clk);
            raw_x = x;
            raw_y = y;
            rgb = pixel_value(x, y, seed);
            @(posedge cap_clk); #0.001;
        end
    endtask

    task region;
        input integer h, v, seed, first, count;
        integer index;
        begin
            for (index = first; index < first + count; index = index + 1)
                sample(h + 128 + index % 256, v + 64 + index / 256, seed);
        end
    endtask

    task complete;
        input integer h, v, seed, sequence;
        input lace, parity, standard_ntsc;
        integer tries, index, x, y;
        reg [31:0] expected_status;
        reg [31:0] previous_read;
        begin
            tries = 0;
            while (status[0] !== 1'b1 && tries < 100) begin
                @(posedge axi_clk); #0.001;
                tries = tries + 1;
            end
            expected_status = (sequence << 16) | (standard_ntsc << 5) |
                              (parity << 4) | (lace << 3) |
                              (arm_toggle << 2) | 1;
            require(status == expected_status, "coherent complete-field metadata");
            require(geometry == ((v << 12) | h), "snapshot crop metadata");
            for (index = 0; index < 1024; index = index + 1) begin
                @(negedge axi_clk);
                previous_read = read_data;
                read_addr = index;
                #0.001;
                require(read_data === previous_read, "RAM read changes only on AXI clock");
                @(posedge axi_clk); #0.001;
                x = h + 128 + index % 256;
                y = v + 64 + index / 256;
                require(read_data === {8'b0, pixel_value(x, y, seed)},
                        "every raw RGB word belongs to one snapshot");
                require(status == expected_status, "snapshot status remains frozen during read");
            end
        end
    endtask

    task invalid_idle;
        begin
            settle;
            require(status[1:0] == 2'b00, "incomplete capture is invalid and idle");
            require(status[2] === arm_toggle, "failed capture retains current arm acknowledgement");
        end
    endtask

    task reset_capture_without_clock;
        begin
            @(negedge cap_clk);
            cap_clock_enabled = 0;
            cap_reset = 1;
            observed_sequence = 0;
            repeat (12) @(posedge axi_clk);
            #0.001;
            require(status[1:0] == 0, "reset invalidates capture with stopped cap clock");
            cap_clock_enabled = 1;
            repeat (3) @(negedge cap_clk);
            cap_reset = 0;
            settle;
            require(status[1:0] == 0, "reset recovery does not rearm old toggle");
        end
    endtask

    initial begin
        repeat (5) @(negedge cap_clk);
        axi_resetn = 1;
        cap_reset = 0;
        settle;
        require(status[1:0] == 0, "startup has no snapshot");

        $display("CASE wait boundary, coherent raw pixels and same-edge metadata");
        arm;
        region(20, 10, 1, 0, 1024);
        require(status[1:0] == 2'b10, "arm ignores remainder of current field");
        boundary(20, 10, 1, 1, 0);
        region(20, 10, 31, 0, 1024);
        complete(20, 10, 31, observed_sequence, 1, 1, 0);

        $display("CASE snapshot remains frozen across fields and configuration changes");
        boundary(21, 11, 0, 0, 1);
        region(21, 11, 42, 0, 1024);
        complete(20, 10, 31, 1, 1, 1, 0);
        arm;
        region(21, 11, 44, 0, 1024);
        require(status[1:0] == 2'b10, "rearm does not accept old valid level");
        boundary(21, 11, 0, 0, 1);
        region(21, 11, 55, 0, 1024);
        complete(21, 11, 55, observed_sequence, 0, 0, 1);

        $display("CASE partial ROI interrupted by frame boundary");
        arm;
        boundary(21, 11, 0, 0, 1);
        region(21, 11, 61, 0, 512);
        boundary(21, 11, 0, 0, 1);
        invalid_idle;
        region(21, 11, 62, 0, 1024);
        invalid_idle;

        $display("CASE mid-frame crop transition waits for next complete field");
        arm;
        boundary(21, 11, 0, 0, 1);
        region(21, 11, 63, 0, 400);
        @(negedge cap_clk);
        crop_h = 25;
        crop_v = 12;
        region(25, 12, 64, 0, 1024);
        require(status[1:0] == 2'b10, "changed crop cannot publish mixed pixels");
        boundary(25, 12, 1, 1, 0);
        region(25, 12, 66, 0, 1024);
        complete(25, 12, 66, observed_sequence, 1, 1, 0);

        $display("CASE missing and repeated coordinates fail without success");
        arm;
        boundary(25, 12, 1, 1, 0);
        region(25, 12, 70, 0, 1);
        region(25, 12, 70, 2, 1022);
        invalid_idle;
        arm;
        boundary(25, 12, 1, 1, 0);
        region(25, 12, 71, 0, 1);
        region(25, 12, 71, 0, 1024);
        invalid_idle;

        $display("CASE capture reset aborts incomplete and frozen snapshots");
        arm;
        boundary(25, 12, 1, 1, 0);
        region(25, 12, 80, 0, 128);
        reset_capture_without_clock;
        boundary(25, 12, 1, 1, 0);
        region(25, 12, 81, 0, 1024);
        invalid_idle;
        arm;
        boundary(25, 12, 1, 1, 0);
        region(25, 12, 82, 0, 1024);
        complete(25, 12, 82, observed_sequence, 1, 1, 0);
        reset_capture_without_clock;
        boundary(25, 12, 1, 1, 0);
        region(25, 12, 83, 0, 1024);
        invalid_idle;

        $display("CASE AXI reset with an outstanding request and high arm level");
        if (!arm_toggle) begin
            arm;
        end else begin
            arm;
            arm;
        end
        boundary(25, 12, 1, 1, 0);
        region(25, 12, 84, 0, 256);
        @(negedge axi_clk); axi_resetn = 0;
        observed_sequence = 0;
        repeat (5) @(negedge axi_clk);
        axi_resetn = 1;
        settle;
        boundary(25, 12, 1, 1, 0);
        region(25, 12, 85, 0, 1024);
        invalid_idle;
        arm;
        boundary(25, 12, 1, 1, 0);
        region(25, 12, 86, 0, 1024);
        complete(25, 12, 86, observed_sequence, 1, 1, 0);

        $display("CASE ROI bounds reject wrap and accept exact coordinate limit");
        arm;
        boundary(1665, 10, 0, 0, 0);
        invalid_idle;
        arm;
        boundary(20, 1981, 0, 0, 0);
        invalid_idle;
        arm;
        boundary(4095, 4095, 0, 0, 0);
        invalid_idle;
        arm;
        boundary(1664, 1980, 0, 0, 0);
        region(1664, 1980, 90, 0, 1024);
        complete(1664, 1980, 90, observed_sequence, 0, 0, 0);

        $display("CASE rearm overtakes pending completion across independent clocks");
        arm;
        boundary(20, 10, 0, 0, 0);
        region(20, 10, 100, 0, 1024);
        // The final write is complete; valid is still crossing to AXI.
        arm;
        settle;
        require(status[1:0] == 2'b10, "prior completion cannot satisfy new arm");
        boundary(20, 10, 0, 0, 0);
        region(20, 10, 101, 0, 1024);
        complete(20, 10, 101, observed_sequence, 0, 0, 0);

        $display("CASE slower AXI clock and opposite field parity");
        @(negedge axi_clk);
        axi_half_period = 23.173;
        arm;
        boundary(19, 9, 1, 0, 1);
        region(19, 9, 111, 0, 1024);
        complete(19, 9, 111, observed_sequence, 1, 0, 1);

        if (failures == 0)
            $display("RESULT PASS calibration capture: %0d checks", checks);
        else
            $display("RESULT FAIL calibration capture: %0d failures / %0d checks", failures, checks);
        $finish;
    end

    initial begin
        #5000000;
        $display("RESULT FAIL calibration capture: timeout");
        $finish;
    end
endmodule
