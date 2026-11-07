`timescale 1ns/1ps
module tb_zz9k_cmd_fetcher;
    reg clk = 0;
    reg resetn = 0;
    always #5 clk = ~clk;

    reg submit_valid;
    wire submit_ready;
    reg [31:0] submit_addr;
    reg [31:0] submit_words;
    wire busy;
    wire cmd_valid;
    reg cmd_ready;
    wire cmd_last;
    wire [31:0] cmd_data;
    wire submission_done;
    reg completion_ack;
    wire read_error;
    wire [31:0] read_error_addr;
    wire [31:0] araddr;
    wire [7:0] arlen;
    wire [2:0] arsize;
    wire [1:0] arburst;
    wire arlock;
    wire [3:0] arcache;
    wire [2:0] arprot;
    wire arvalid;
    reg arready;
    reg [63:0] rdata;
    reg [1:0] rresp;
    reg rlast;
    reg rvalid;
    wire rready;

    reg [31:0] memory [0:4095];
    reg [31:0] expected [0:31];
    reg expected_last [0:31];
    integer expected_count;
    integer received_count;
    integer completed_count;
    integer i;
    integer timeout_count;
    reg [15:0] random_state;
    reg read_active;
    reg [31:0] read_addr;
    reg [8:0] read_beats_left;
    integer read_delay;
    reg [31:0] error_address;

    zz9k_cmd_fetcher dut (
        .aclk(clk), .aresetn(resetn),
        .submit_valid(submit_valid), .submit_ready(submit_ready),
        .submit_addr(submit_addr), .submit_words(submit_words), .busy(busy),
        .m_cmd_axis_tvalid(cmd_valid), .m_cmd_axis_tready(cmd_ready),
        .m_cmd_axis_tlast(cmd_last), .m_cmd_axis_tdata(cmd_data),
        .submission_done(submission_done), .completion_ack(completion_ack),
        .read_error(read_error), .read_error_addr(read_error_addr),
        .m_axi_araddr(araddr), .m_axi_arlen(arlen), .m_axi_arsize(arsize),
        .m_axi_arburst(arburst), .m_axi_arlock(arlock), .m_axi_arcache(arcache),
        .m_axi_arprot(arprot), .m_axi_arvalid(arvalid), .m_axi_arready(arready),
        .m_axi_rdata(rdata), .m_axi_rresp(rresp), .m_axi_rlast(rlast),
        .m_axi_rvalid(rvalid), .m_axi_rready(rready)
    );

    function [31:0] word_at;
        input [31:0] address;
        begin
            word_at = memory[address[13:2]];
        end
    endfunction

    task submit;
        input [31:0] address;
        input [31:0] words;
        begin
            while (!submit_ready) @(posedge clk);
            submit_addr <= address;
            submit_words <= words;
            submit_valid <= 1'b1;
            @(posedge clk);
            submit_valid <= 1'b0;
        end
    endtask

    always @(posedge clk) begin
        random_state <= {random_state[14:0], random_state[15] ^ random_state[13] ^ random_state[12] ^ random_state[10]};
        arready <= random_state[0] | random_state[3];
        cmd_ready <= random_state[1] | random_state[5];
        completion_ack <= 1'b0;

        if (arvalid && arready) begin
            if (arsize != 3'b011 || arburst != 2'b01 || arlen > 8'd15)
                $fatal(1, "invalid AXI burst");
            if ((araddr[11:0] + ((arlen + 1) << 3)) > 4096)
                $fatal(1, "burst crossed 4 KiB boundary");
            read_active <= 1'b1;
            read_addr <= araddr;
            read_beats_left <= arlen + 1;
            read_delay <= random_state[8:6];
        end

        if (read_active && !rvalid) begin
            if (read_delay != 0)
                read_delay <= read_delay - 1;
            else begin
                rvalid <= 1'b1;
                rdata <= {word_at(read_addr + 4), word_at(read_addr)};
                rresp <= (read_addr == error_address) ? 2'b10 : 2'b00;
                rlast <= (read_beats_left == 1);
            end
        end
        if (rvalid && rready) begin
            rvalid <= 1'b0;
            if (read_beats_left == 1) begin
                read_active <= 1'b0;
                rlast <= 1'b0;
            end else begin
                read_addr <= read_addr + 8;
                read_beats_left <= read_beats_left - 1;
            end
        end

        if (cmd_valid && cmd_ready) begin
            if (received_count >= expected_count || cmd_data !== expected[received_count])
                $fatal(1, "word %0d: got %08x", received_count, cmd_data);
            received_count <= received_count + 1;
            if (cmd_last != expected_last[received_count])
                $fatal(1, "bad tlast on word %0d", received_count);
        end
        if (submission_done) begin
            completed_count <= completed_count + 1;
            completion_ack <= 1'b1;
        end
    end

    initial begin
        submit_valid = 0;
        submit_addr = 0;
        submit_words = 0;
        cmd_ready = 0;
        completion_ack = 0;
        arready = 0;
        rdata = 0;
        rresp = 0;
        rlast = 0;
        rvalid = 0;
        random_state = 16'h1ACE;
        read_active = 0;
        read_addr = 0;
        read_beats_left = 0;
        read_delay = 0;
        error_address = 32'hFFFF_FFFF;
        expected_count = 0;
        received_count = 0;
        completed_count = 0;
        for (i = 0; i < 4096; i = i + 1)
            memory[i] = 32'hA500_0000 + i;

        repeat (4) @(posedge clk);
        resetn = 1;

        // 0x0ff0 forces a short burst before the 4 KiB boundary.  Nine words
        // exercises the odd final 64-bit beat; the second submit proves queueing.
        for (i = 0; i < 9; i = i + 1) begin
            expected[expected_count] = word_at(32'h0ff0 + (i << 2));
            expected_last[expected_count] = (i == 8);
            expected_count = expected_count + 1;
        end
        submit(32'h0000_0ff0, 9);
        for (i = 0; i < 5; i = i + 1) begin
            expected[expected_count] = word_at(32'h0000_2000 + (i << 2));
            expected_last[expected_count] = (i == 4);
            expected_count = expected_count + 1;
        end
        submit(32'h0000_2000, 5);

        timeout_count = 0;
        while (completed_count != 2 && timeout_count < 20000) begin
            @(posedge clk);
            timeout_count = timeout_count + 1;
        end
        if (completed_count != 2 || received_count != expected_count)
            $fatal(1, "queued submissions did not complete");

        // An RRESP error must terminate the submission and report its beat address.
        error_address = 32'h0000_3000;
        submit(32'h0000_3000, 7);
        timeout_count = 0;
        while (!read_error && timeout_count < 20000) begin
            @(posedge clk);
            timeout_count = timeout_count + 1;
        end
        if (!read_error || read_error_addr != error_address)
            $fatal(1, "RRESP error not reported at failing address");
        repeat (10) @(posedge clk);
        if (received_count != expected_count)
            $fatal(1, "error submission leaked command words");
        $display("PASS: AXI bursts, boundary, odd words, queue, backpressure, and error response");
        $finish;
    end
endmodule
