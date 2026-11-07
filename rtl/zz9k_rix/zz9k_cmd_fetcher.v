// SPDX-License-Identifier: GPL-3.0-or-later
// AXI4 read master which turns queued DDR command buffers into RasterIX words.
//
// The renderer memory master and this fetcher share one HP read port through a
// single in-order read-data channel (crossbar + AXI3 protocol converter).  A
// master that parks undelivered read beats on that channel while it waits for
// the renderer therefore deadlocks the renderer itself: the renderer stalls on
// its own framebuffer/texture reads, stops consuming command words, and the
// fetcher never frees the channel.  This module thus never withdraws rready
// while beats of an issued burst are outstanding: bursts are only issued when
// the whole burst fits into the receive FIFO, so every in-flight beat always
// has reserved space.  Mid-burst backpressure from the renderer-side consumer
// only delays the next burst, it never holds the shared channel.
`timescale 1ns/1ps
module zz9k_cmd_fetcher (
    input  wire        aclk,
    input  wire        aresetn,

    input  wire        submit_valid,
    output wire        submit_ready,
    input  wire [31:0] submit_addr,
    input  wire [31:0] submit_words,
    output wire        busy,

    output wire        m_cmd_axis_tvalid,
    input  wire        m_cmd_axis_tready,
    output wire        m_cmd_axis_tlast,
    output wire [31:0] m_cmd_axis_tdata,
    output reg         submission_done,
    input  wire        completion_ack,
    output reg         read_error,
    output reg  [31:0] read_error_addr,
    output wire [31:0] m_axi_araddr,
    output wire [7:0]  m_axi_arlen,
    output wire [2:0]  m_axi_arsize,
    output wire [1:0]  m_axi_arburst,
    output wire        m_axi_arlock,
    output wire [3:0]  m_axi_arcache,
    output wire [2:0]  m_axi_arprot,
    output wire        m_axi_arvalid,
    input  wire        m_axi_arready,
    input  wire [63:0] m_axi_rdata,
    input  wire [1:0]  m_axi_rresp,
    input  wire        m_axi_rlast,
    input  wire        m_axi_rvalid,
    output wire        m_axi_rready,

    // Debug taps for the timeout CSR dump.
    output wire [2:0]  dbg_state,
    output wire [4:0]  dbg_fifo_count
);
    // Read side (producer) states.
    localparam [2:0] P_IDLE = 3'd0;
    localparam [2:0] P_AR   = 3'd1;
    localparam [2:0] P_RX   = 3'd2;
    localparam [2:0] P_ERR  = 3'd3;
    // Renderer side (consumer) states.
    localparam [1:0] C_IDLE = 2'd0;
    localparam [1:0] C_LOW  = 2'd1;
    localparam [1:0] C_HIGH = 2'd2;
    localparam [1:0] C_HOLD = 2'd3;

    localparam [4:0] FIFO_DEPTH = 5'd16;

    reg [2:0] p_state;
    reg [1:0] c_state;

    reg [31:0] queue_addr [0:1];
    reg [31:0] queue_words [0:1];
    reg        queue_write_ptr;
    reg        queue_read_ptr;
    reg [1:0]  queue_count;

    reg [63:0] fifo_mem [0:15];
    reg [3:0]  fifo_wptr;
    reg [3:0]  fifo_rptr;
    reg [4:0]  fifo_count;
    reg [31:0] total_words;  // words of the active submission
    reg [31:0] forwarded;    // words handed to the renderer
    reg [31:0] rx_words_left; // words of the active submission not yet received
    reg [31:0] next_burst_addr;
    reg [31:0] burst_addr;
    reg [4:0]  burst_beat_index;

    wire        queue_push;
    wire        queue_pop;
    wire [31:0] words_to_4k;
    wire [31:0] beats_for_words;
    wire [31:0] beats_for_boundary;
    wire [31:0] beats_for_space;
    wire [31:0] selected_burst_beats;
    wire        fifo_push;
    wire        fifo_pop;
    wire [4:0]  fifo_free = FIFO_DEPTH - fifo_count;
    wire [63:0] fifo_head = fifo_mem[fifo_rptr];
    // The consumer presents word #forwarded in both states: entering C_HIGH
    // already counted the forwarded low word.
    wire [31:0] c_present_index = forwarded;
    wire        c_high_word_valid = (c_state == C_LOW) && ((forwarded + 32'd1) < total_words);
    wire        c_word_handshake = m_cmd_axis_tvalid && m_cmd_axis_tready;

    assign submit_ready = (queue_count != 2);
    assign queue_push = submit_valid && submit_ready;
    assign queue_pop = (p_state == P_IDLE) && (c_state == C_IDLE) && (queue_count != 0);
    assign busy = (p_state != P_IDLE) || (c_state != C_IDLE) || (queue_count != 0);

    // The buffer base is required to be eight-byte aligned.  A 64-bit beat
    // cannot cross a 4 KiB boundary, AXI limits this master to 16 beats, and a
    // burst is only issued when the receive FIFO can hold all of its beats.
    assign words_to_4k = (32'd4096 - {20'd0, next_burst_addr[11:0]}) >> 2;
    assign beats_for_words = (rx_words_left + 1) >> 1;
    assign beats_for_boundary = words_to_4k >> 1;
    assign beats_for_space = {27'd0, fifo_free};
    assign selected_burst_beats =
        (beats_for_words < beats_for_boundary) ?
        ((beats_for_words < beats_for_space) ?
            ((beats_for_words < 16) ? beats_for_words : 32'd16) :
            ((beats_for_space < 16) ? beats_for_space : 32'd16)) :
        ((beats_for_boundary < beats_for_space) ?
            ((beats_for_boundary < 16) ? beats_for_boundary : 32'd16) :
            ((beats_for_space < 16) ? beats_for_space : 32'd16));

    assign m_axi_araddr = next_burst_addr;
    assign m_axi_arlen = selected_burst_beats[7:0] - 1'b1;
    assign m_axi_arsize = 3'b011; // 64-bit beats
    assign m_axi_arburst = 2'b01; // INCR
    assign m_axi_arlock = 1'b0;
    assign m_axi_arcache = 4'b0011;
    assign m_axi_arprot = 3'b000;
    assign m_axi_arvalid = (p_state == P_AR) && (selected_burst_beats >= 32'd1) &&
                           (selected_burst_beats <= 32'd16);
    // Reservation makes rready safe: every beat of the current burst already
    // has FIFO space, so rready stays asserted through rlast even when the
    // renderer-side consumer is completely stalled.
    assign m_axi_rready = (p_state == P_RX) || (p_state == P_ERR);
    assign fifo_push = (p_state == P_RX) && m_axi_rvalid && m_axi_rready;

    assign m_cmd_axis_tvalid = (c_state == C_LOW) || (c_state == C_HIGH);
    assign m_cmd_axis_tdata = (c_state == C_LOW) ? fifo_head[31:0] : fifo_head[63:32];
    assign m_cmd_axis_tlast = (c_present_index + 32'd1 == total_words);
    assign fifo_pop = c_word_handshake &&
                      ((c_state == C_HIGH) || ((c_state == C_LOW) && !c_high_word_valid));

    assign dbg_state = p_state;
    assign dbg_fifo_count = fifo_count;

    always @(posedge aclk) begin
        if (!aresetn) begin
            p_state <= P_IDLE;
            c_state <= C_IDLE;
            queue_write_ptr <= 1'b0;
            queue_read_ptr <= 1'b0;
            queue_count <= 2'd0;
            fifo_wptr <= 4'd0;
            fifo_rptr <= 4'd0;
            fifo_count <= 5'd0;
            total_words <= 32'd0;
            forwarded <= 32'd0;
            rx_words_left <= 32'd0;
            next_burst_addr <= 32'd0;
            burst_addr <= 32'd0;
            burst_beat_index <= 5'd0;
            submission_done <= 1'b0;
            read_error <= 1'b0;
            read_error_addr <= 32'd0;
        end else begin
            submission_done <= 1'b0;
            read_error <= 1'b0;

            if (queue_push) begin
                queue_addr[queue_write_ptr] <= submit_addr;
                queue_words[queue_write_ptr] <= submit_words;
                queue_write_ptr <= ~queue_write_ptr;
            end
            if (queue_pop) begin
                queue_read_ptr <= ~queue_read_ptr;
                next_burst_addr <= queue_addr[queue_read_ptr];
                total_words <= queue_words[queue_read_ptr];
                rx_words_left <= queue_words[queue_read_ptr];
                forwarded <= 32'd0;
                p_state <= P_AR;
            end
            case ({queue_push, queue_pop})
                2'b10: queue_count <= queue_count + 1'b1;
                2'b01: queue_count <= queue_count - 1'b1;
                default: begin end
            endcase

            case ({fifo_push, fifo_pop})
                2'b10: begin
                    fifo_mem[fifo_wptr] <= m_axi_rdata;
                    fifo_wptr <= fifo_wptr + 1'b1;
                    fifo_count <= fifo_count + 1'b1;
                end
                2'b01: begin
                    fifo_rptr <= fifo_rptr + 1'b1;
                    fifo_count <= fifo_count - 1'b1;
                end
                2'b11: begin
                    fifo_mem[fifo_wptr] <= m_axi_rdata;
                    fifo_wptr <= fifo_wptr + 1'b1;
                    fifo_rptr <= fifo_rptr + 1'b1;
                end
                default: begin end
            endcase

            case (p_state)
                P_AR: if (m_axi_arvalid && m_axi_arready) begin
                    burst_addr <= next_burst_addr;
                    burst_beat_index <= 5'd0;
                    next_burst_addr <= next_burst_addr +
                                       (selected_burst_beats << 3);
                    p_state <= P_RX;
                end
                P_RX: if (m_axi_rvalid && m_axi_rready) begin
                    burst_beat_index <= burst_beat_index + 1'b1;
                    if (m_axi_rresp != 2'b00) begin
                        read_error <= 1'b1;
                        read_error_addr <= burst_addr +
                            ({27'd0, burst_beat_index} << 3);
                        // Abort the submission: drain the burst, drop the
                        // consumer, and flush any beats still buffered.
                        c_state <= C_IDLE;
                        fifo_rptr <= fifo_wptr;
                        fifo_count <= 5'd0;
                        p_state <= m_axi_rlast ? P_IDLE : P_ERR;
                    end else begin
                        rx_words_left <= (rx_words_left > 32'd2) ?
                            (rx_words_left - 32'd2) : 32'd0;
                        if (m_axi_rlast)
                            p_state <= (rx_words_left > 32'd2) ? P_AR : P_IDLE;
                    end
                end
                P_ERR: if (m_axi_rvalid && m_axi_rready && m_axi_rlast)
                    p_state <= P_IDLE;
                default: begin end
            endcase

            case (c_state)
                C_IDLE: if (fifo_count != 5'd0)
                    c_state <= C_LOW;
                C_LOW: if (c_word_handshake) begin
                    forwarded <= forwarded + 32'd1;
                    if (c_high_word_valid) begin
                        c_state <= C_HIGH;
                    end else begin
                        // Single-word final beat: this word completes the
                        // submission (m_cmd_axis_tlast was asserted).
                        submission_done <= 1'b1;
                        c_state <= C_HOLD;
                    end
                end
                C_HIGH: if (c_word_handshake) begin
                    forwarded <= forwarded + 32'd1;
                    if (forwarded + 32'd1 == total_words) begin
                        submission_done <= 1'b1;
                        c_state <= C_HOLD;
                    end else begin
                        c_state <= C_IDLE;
                    end
                end
                C_HOLD: if (completion_ack)
                    c_state <= C_IDLE;
                default: begin end
            endcase
        end
    end
endmodule
