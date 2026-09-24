`timescale 1ns/1ps
/* Protocol and row-ownership tests for the extracted production writeback.
 * Ownership cases preserve the two banks. Separate protocol cases overwrite
 * the addressed RAM word after WVALID, testing the beat hold during recovery.
 * They do not promise retention of unpresented words under unbounded stalls.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
module videocap_writeback_tb;
    localparam [31:0] BASE = 32'h08000000;
    localparam integer PITCH = 32;
    reg clk = 0, resetn = 0, capture_ready = 0;
    reg [11:0] token = 0;
    reg awready = 0, wready = 0;
    wire [31:0] awaddr, wdata;
    wire awvalid, wvalid, wlast;
    wire [3:0] wstrb;
    wire [7:0] awlen;
    wire [2:0] awsize;
    wire [1:0] awburst;
    wire [11:0] memory_address;
    wire memory_bank;
    wire [3:0] state;
    wire [4:0] beat;
    reg [31:0] memory [0:127];
    reg [31:0] memory_data = 0;
    integer checks = 0, failures = 0;
    integer total_aw = 0, total_w = 0, row_bursts = 0;
    integer expected_row = 0, expected_bank = 0;
    integer burst_row = 0, burst_bank = 0, burst_x = 0, burst_beats = 0;
    reg outstanding = 0;
    reg previous_aw_stall = 0, previous_w_stall = 0;
    reg [31:0] previous_awaddr, previous_wdata;
    reg previous_wlast;
    reg [3:0] previous_wstrb;
    integer before_aw, before_w, tries;
    integer axi_cycle = 0, burst_aw_cycle = 0;
    reg burst_unstalled = 0;

    always #5 clk = ~clk;
    always @(posedge clk)
        memory_data <= memory[{memory_bank, memory_address[5:0]}];

    extracted_videocap_writeback dut (
        .S_AXI_ACLK(clk), .m01_axi_aresetn(resetn),
        .vcap_capture_ready_axi(capture_ready), .vcap_line_payload_axi(token),
        .videocap_mode(1'b1), .videocap_address(BASE), .videocap_pitch(12'd32),
        .videocap_control_applied_full_width(1'b1),
        .m01_axi_awready(awready), .m01_axi_wready(wready), .vcap_rdata(memory_data),
        .m01_axi_awaddr(awaddr), .m01_axi_awvalid(awvalid),
        .m01_axi_wdata(wdata), .m01_axi_wstrb(wstrb), .m01_axi_wvalid(wvalid),
        .m01_axi_wlast(wlast), .m01_axi_awlen(awlen), .m01_axi_awsize(awsize),
        .m01_axi_awburst(awburst), .memory_address(memory_address),
        .memory_bank(memory_bank), .state(state), .beat(beat)
    );

    task require;
        input condition;
        input [767:0] description;
        begin
            checks = checks + 1;
            if (condition !== 1'b1) begin
                failures = failures + 1;
                if (failures < 30)
                    $display("MISMATCH %0s t=%0t state=%0d beat=%0d aw=%08x data=%08x",
                        description, $time, state, beat, awaddr, wdata);
            end
        end
    endtask

    function [31:0] word;
        input integer row_number, bank_number, x;
        begin word = 32'hca000000 | (bank_number << 20) | (row_number << 8) | x; end
    endfunction

    task load_bank;
        input integer row_number, bank_number;
        integer x;
        begin
            for (x = 0; x < 64; x = x + 1)
                memory[bank_number * 64 + x] = word(row_number, bank_number, x);
        end
    endtask

    task clocks;
        input integer count;
        begin repeat (count) begin @(posedge clk); #0.001; end end
    endtask

    task publish_row;
        input integer row_number, bank_number;
        begin
            @(negedge clk);
            token = {~token[11], bank_number[0], row_number[9:0]};
        end
    endtask

    always @(posedge clk) begin
        axi_cycle = axi_cycle + 1;
        if (!resetn) begin
            previous_aw_stall = 0;
            previous_w_stall = 0;
            outstanding = 0;
        end else begin
            if (previous_aw_stall) begin
                require(awvalid, "AWVALID survives backpressure and capture loss");
                require(awaddr == previous_awaddr, "pending AW address remains frozen");
            end
            if (previous_w_stall) begin
                require(wvalid, "WVALID survives backpressure and capture loss");
                require(wdata == previous_wdata, "WDATA remains stable on a stalled valid beat");
                require(wlast == previous_wlast && wstrb == previous_wstrb,
                    "WLAST and WSTRB remain stable on a stalled valid beat");
            end
            previous_aw_stall = awvalid && !awready;
            previous_w_stall = wvalid && !wready;
            previous_awaddr = awaddr;
            previous_wdata = wdata;
            previous_wlast = wlast;
            previous_wstrb = wstrb;

            if (awvalid && awready) begin
                require(!outstanding, "only one write-data burst is outstanding");
                require(awlen == 15 && awsize == 2 && awburst == 1,
                    "production AW attributes describe sixteen incrementing 32-bit beats");
                require(awaddr == BASE + 4 * (expected_row * PITCH + row_bursts * 16),
                    "AW address belongs to the frozen row and sequential burst");
                outstanding = 1;
                burst_row = expected_row;
                burst_bank = expected_bank;
                burst_x = row_bursts * 16;
                burst_beats = 0;
                burst_aw_cycle = axi_cycle;
                burst_unstalled = wready;
                row_bursts = row_bursts + 1;
                total_aw = total_aw + 1;
            end
            if (outstanding && !wready) burst_unstalled = 0;
            if (wvalid && wready) begin
                require(outstanding, "no write-data beat without an accepted address");
                require(burst_beats < 16, "accepted burst has no extra data beat");
                require(wdata == word(burst_row, burst_bank, burst_x + burst_beats),
                    "accepted data comes from the frozen bank, row and pixel address");
                require(wstrb == 15 && wlast == (burst_beats == 15),
                    "only the sixteenth accepted beat carries WLAST");
                burst_beats = burst_beats + 1;
                total_w = total_w + 1;
                if (burst_beats == 16) begin
                    if (burst_unstalled)
                        require(axi_cycle - burst_aw_cycle == 32,
                            "unstalled burst retains the two-clock-per-word cadence");
                    outstanding = 0;
                end
            end
        end
    end

    task start_case;
        input integer row_number, bank_number;
        begin
            @(negedge clk);
            resetn = 0; capture_ready = 0; awready = 0; wready = 0;
            clocks(4);
            @(negedge clk); resetn = 1; capture_ready = 1;
            clocks(2); /* state 4 -> state 0 -> idle state 2 */
            @(negedge clk); capture_ready = 0;
            clocks(4); /* establish the real invalid-capture sentinel state */
            require(state == 2 && !awvalid && !wvalid, "invalid capture waits in the idle FSM state");
            expected_row = row_number; expected_bank = bank_number; row_bursts = 0;
            before_aw = total_aw; before_w = total_w;
            load_bank(row_number, bank_number);
            load_bank(row_number + 30, !bank_number);
            @(negedge clk); capture_ready = 1;
            clocks(4);
            require(!awvalid, "readiness alone cannot replay an old row before a fresh token");
            publish_row(row_number, bank_number);
            tries = 0;
            while (!awvalid && tries < 30) begin clocks(1); tries = tries + 1; end
            require(awvalid && state == 3, "new completed row produces an address request");
        end
    endtask

    task wait_wvalid;
        begin
            tries = 0;
            while (!wvalid && tries < 10) begin clocks(1); tries = tries + 1; end
            require(wvalid && state == 1, "accepted AW reaches a valid data beat");
        end
    endtask

    task drain_and_check_invalid;
        begin
            @(negedge clk); awready = 1; wready = 1;
            tries = 0;
            while ((total_w - before_w) < 16 && tries < 100) begin
                clocks(1); tries = tries + 1;
            end
            require(total_aw - before_aw == 1 && total_w - before_w == 16,
                "the outstanding burst drains exactly sixteen beats after loss");
            clocks(8);
            require(state == 2 && !awvalid && !wvalid,
                "state 2 starts no further burst while capture is invalid");
            require(total_aw - before_aw == 1, "clock loss abandons the remaining old-row bursts");
            @(negedge clk); capture_ready = 1;
            clocks(10);
            require(!awvalid && total_aw - before_aw == 1,
                "recovery waits for a new clean completed-row token");
        end
    endtask

    task recover_before_drain;
        begin
            @(negedge clk); capture_ready = 0;
            clocks(5);
            @(negedge clk); capture_ready = 1;
            clocks(5);
            publish_row(expected_row + 30, !expected_bank);
            clocks(5);
            @(negedge clk); awready = 1; wready = 1;
            tries = 0;
            while (total_w - before_w < 16 && tries < 100) begin
                clocks(1); tries = tries + 1;
            end
            @(negedge clk); awready = 0; wready = 0;
            clocks(10);
            require(total_w - before_w == 16 && !outstanding,
                "already-presented address drains normally despite readiness recovery");
            require(!awvalid && total_aw - before_aw == 1,
                "loss observed outside state 2 still abandons the old row until a new token");
            // The row that arrived while the lost burst drained was baselined.
            // A token published after invalidation must restart at its own x=0.
            expected_row = expected_row + 1;
            expected_bank = !expected_bank;
            row_bursts = 0;
            load_bank(expected_row, expected_bank);
            publish_row(expected_row, expected_bank);
            @(negedge clk); awready = 1; wready = 1;
            tries = 0;
            while (total_w - before_w < 48 && tries < 150) begin
                clocks(1); tries = tries + 1;
            end
            require(total_aw - before_aw == 3 && total_w - before_w == 48,
                "post-invalidation token resumes both bursts at the new row origin");
        end
    endtask

    task overwrite_stalled_word;
        input integer row_number, bank_number, word_index;
        reg [31:0] presented_word;
        reg presented_last;
        integer selected_address, mutation;
        begin
            start_case(row_number, bank_number);
            @(negedge clk); awready = 1; wready = (word_index != 0);
            tries = 0;
            while (total_w - before_w < word_index && tries < 80) begin
                clocks(1); tries = tries + 1;
            end
            if (word_index != 0) begin @(negedge clk); wready = 0; end
            wait_wvalid();
            require(beat == word_index, "selected first/middle/last word is valid before overwrite");
            presented_word = wdata;
            presented_last = wlast;
            selected_address = memory_bank * 64 + memory_address;
            require(presented_word == word(row_number, bank_number, word_index),
                "the newly presented word has the correct synchronous RAM address");

            // Change RAM before the first stalled VALID edge. Its synchronous
            // read changes at that edge, while the skid must capture the word
            // already presented before the edge. Later writes must not reload it.
            for (mutation = 0; mutation < 3; mutation = mutation + 1) begin
                @(negedge clk);
                capture_ready = (mutation == 2);
                memory[selected_address] = 32'hdeaf0000 + mutation;
                clocks(1);
                require(memory_data == 32'hdeaf0000 + mutation,
                    "the actual selected RAM output changed during backpressure");
                require(wvalid && wdata == presented_word && wlast == presented_last,
                    "stalled AXI beat is held across RAM overwrite and readiness recovery");
            end
            clocks(3);
            // Do not restore RAM: acceptance must still use the original word.
            // Other words retain the distinctive row/bank/x sequence checked
            // by the common monitor, including the word following the hold.
            @(negedge clk); wready = 1;
            tries = 0;
            while (total_w - before_w < 16 && tries < 100) begin
                clocks(1); tries = tries + 1;
            end
            @(negedge clk); awready = 0; wready = 0;
            clocks(8);
            require(total_w - before_w == 16 && !outstanding,
                "overwritten stalled word still completes exactly sixteen accepted beats");
            require(!awvalid && total_aw - before_aw == 1,
                "recovered capture with overwritten RAM awaits a fresh row token");
        end
    endtask

    initial begin
        #1000000;
        $display("RESULT FAIL writeback: watchdog expired");
        $finish;
    end

    initial begin
        $display("CASE loss with AW pending in state 3 and W stalled before/after loss");
        start_case(5, 1);
        /* Let a newer completed-row token advance the handoff pipeline
         * before loss, while the presented old-row AW remains stalled. */
        publish_row(35, 0);
        clocks(6);
        @(negedge clk); capture_ready = 0;
        clocks(5);
        require(awvalid && state == 3, "pending AW survives source loss without withdrawing VALID");
        @(negedge clk); awready = 1;
        wait_wvalid();
        clocks(5);
        drain_and_check_invalid();

        /* Verify that an actually fresh row resumes with its own base/bank. */
        expected_row = 12; expected_bank = 0; row_bursts = 0;
        load_bank(12, 0);
        publish_row(12, 0);
        tries = 0;
        while (total_w - before_w < 48 && tries < 150) begin clocks(1); tries = tries + 1; end
        require(total_aw - before_aw == 3 && total_w - before_w == 48,
            "fresh row resumes as two complete bursts from its new bank");

        $display("CASE loss in synchronous RAM-read bubble state 5");
        start_case(7, 0);
        @(negedge clk); awready = 1;
        clocks(1);
        require(state == 5 && !wvalid, "AW handshake enters the RAM-read bubble");
        @(negedge clk); capture_ready = 0;
        publish_row(37, 1);
        wait_wvalid();
        clocks(5);
        drain_and_check_invalid();

        $display("CASE loss with middle WVALID stalled in state 1");
        start_case(9, 1);
        @(negedge clk); awready = 1; wready = 1;
        tries = 0;
        while (total_w - before_w < 7 && tries < 50) begin clocks(1); tries = tries + 1; end
        @(negedge clk); wready = 0;
        wait_wvalid();
        require(beat == 7 && !wlast, "middle beat selected for pre-loss backpressure");
        clocks(3);
        @(negedge clk); capture_ready = 0;
        publish_row(39, 0);
        clocks(5);
        drain_and_check_invalid();

        $display("CASE loss with the final WLAST beat stalled");
        start_case(11, 0);
        @(negedge clk); awready = 1; wready = 1;
        tries = 0;
        while (total_w - before_w < 15 && tries < 80) begin clocks(1); tries = tries + 1; end
        @(negedge clk); wready = 0;
        wait_wvalid();
        require(beat == 15 && wlast, "last beat selected for pre-loss backpressure");
        clocks(3);
        @(negedge clk); capture_ready = 0;
        clocks(5);
        drain_and_check_invalid();

        $display("CASE readiness recovers before a previously stalled AW drains");
        start_case(13, 1);
        recover_before_drain();

        $display("CASE readiness recovers after loss in state 5 before data drains");
        start_case(15, 0);
        @(negedge clk); awready = 1;
        clocks(1);
        require(state == 5 && !wvalid, "recovery case begins in the RAM-read bubble");
        recover_before_drain();

        $display("CASE readiness recovers after loss on a stalled middle W beat");
        start_case(17, 1);
        @(negedge clk); awready = 1; wready = 1;
        tries = 0;
        while (total_w - before_w < 7 && tries < 50) begin clocks(1); tries = tries + 1; end
        @(negedge clk); wready = 0;
        wait_wvalid();
        recover_before_drain();

        $display("CASE selected RAM word overwritten on first stalled WVALID edge");
        overwrite_stalled_word(19, 0, 0);
        $display("CASE selected middle RAM word overwritten across loss and recovery");
        overwrite_stalled_word(21, 1, 7);
        $display("CASE selected final RAM word overwritten while WLAST is stalled");
        overwrite_stalled_word(23, 0, 15);

        if (failures)
            $display("RESULT FAIL writeback: %0d failures / %0d checks", failures, checks);
        else
            $display("RESULT PASS writeback: %0d checks", checks);
        $finish;
    end
endmodule
