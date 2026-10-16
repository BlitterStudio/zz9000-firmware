// SPDX-License-Identifier: GPL-3.0-or-later
// AXI4 read master which turns queued DDR command buffers into RasterIX words.
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
    output reg [31:0]  read_error_addr,
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
    output wire        m_axi_rready
);
    localparam [2:0] ST_IDLE  = 3'd0;
    localparam [2:0] ST_AR    = 3'd1;
    localparam [2:0] ST_R     = 3'd2;
    localparam [2:0] ST_LOW   = 3'd3;
    localparam [2:0] ST_HIGH  = 3'd4;
    localparam [2:0] ST_DRAIN = 3'd5;
    localparam [2:0] ST_HOLD  = 3'd6;

    reg [2:0] state;
    reg [31:0] queue_addr [0:1];
    reg [31:0] queue_words [0:1];
    reg        queue_write_ptr;
    reg        queue_read_ptr;
    reg [1:0]  queue_count;
    reg [31:0] words_left;
    reg [31:0] next_burst_addr;
    reg [31:0] burst_addr;
    reg [4:0]  burst_beat_index;
    reg [63:0] read_data;
    reg        read_last;
    reg        high_word_valid;

    wire queue_push;
    wire queue_pop;
    wire [31:0] words_to_4k;
    wire [31:0] beats_for_words;
    wire [31:0] beats_for_boundary;
    wire [31:0] selected_burst_beats;

    assign submit_ready = (queue_count != 2);
    assign queue_push = submit_valid && submit_ready;
    assign queue_pop = (state == ST_IDLE) && (queue_count != 0);
    assign busy = (state != ST_IDLE) || (queue_count != 0);

    // The buffer base is required to be eight-byte aligned.  A 64-bit beat
    // cannot cross a 4 KiB boundary, and AXI limits this master to 16 beats.
    assign words_to_4k = (32'd4096 - {20'd0, next_burst_addr[11:0]}) >> 2;
    assign beats_for_words = (words_left + 1) >> 1;
    assign beats_for_boundary = words_to_4k >> 1;
    assign selected_burst_beats =
        (beats_for_words < beats_for_boundary) ?
        ((beats_for_words < 16) ? beats_for_words : 16) :
        ((beats_for_boundary < 16) ? beats_for_boundary : 16);

    assign m_axi_araddr = next_burst_addr;
    assign m_axi_arlen = selected_burst_beats[7:0] - 1'b1;
    assign m_axi_arsize = 3'b011; // 64-bit beats
    assign m_axi_arburst = 2'b01; // INCR
    assign m_axi_arlock = 1'b0;
    assign m_axi_arcache = 4'b0011;
    assign m_axi_arprot = 3'b000;
    assign m_axi_arvalid = (state == ST_AR);
    assign m_axi_rready = (state == ST_R) || (state == ST_DRAIN);

    assign m_cmd_axis_tvalid = (state == ST_LOW) || (state == ST_HIGH);
    assign m_cmd_axis_tdata = (state == ST_LOW) ? read_data[31:0] : read_data[63:32];
    assign m_cmd_axis_tlast = (words_left == 1);

    always @(posedge aclk) begin
        if (!aresetn) begin
            state <= ST_IDLE;
            queue_write_ptr <= 1'b0;
            queue_read_ptr <= 1'b0;
            queue_count <= 2'd0;
            words_left <= 32'd0;
            next_burst_addr <= 32'd0;
            burst_addr <= 32'd0;
            burst_beat_index <= 5'd0;
            read_data <= 64'd0;
            read_last <= 1'b0;
            high_word_valid <= 1'b0;
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
                words_left <= queue_words[queue_read_ptr];
                state <= ST_AR;
            end
            case ({queue_push, queue_pop})
                2'b10: queue_count <= queue_count + 1'b1;
                2'b01: queue_count <= queue_count - 1'b1;
                default: begin end
            endcase

            case (state)
                ST_AR: if (m_axi_arready) begin
                    burst_addr <= next_burst_addr;
                    burst_beat_index <= 5'd0;
                    next_burst_addr <= next_burst_addr + (selected_burst_beats << 3);
                    state <= ST_R;
                end
                ST_R: if (m_axi_rvalid) begin
                    if (m_axi_rresp != 2'b00) begin
                        read_error <= 1'b1;
                        read_error_addr <= burst_addr + ({27'd0, burst_beat_index} << 3);
                        state <= m_axi_rlast ? ST_IDLE : ST_DRAIN;
                    end else begin
                        read_data <= m_axi_rdata;
                        read_last <= m_axi_rlast;
                        high_word_valid <= (words_left > 1);
                        state <= ST_LOW;
                    end
                    burst_beat_index <= burst_beat_index + 1'b1;
                end
                // submit_words is authoritative: the final AXI beat can contain
                // either one or two 32-bit RasterIX words.
                ST_LOW: if (m_cmd_axis_tready) begin
                    if (high_word_valid) begin
                        state <= ST_HIGH;
                    end else if (words_left == 1) begin
                        submission_done <= 1'b1;
                        state <= ST_HOLD;
                    end else if (read_last) begin
                        state <= ST_R;
                    end
                end
                ST_HIGH: if (m_cmd_axis_tready) begin
                    words_left <= words_left - 1'b1;
                    if (words_left == 2) begin
                        submission_done <= 1'b1;
                        state <= ST_HOLD;
                    end else if (read_last) begin
                        state <= ST_AR;
                    end else begin
                        state <= ST_R;
                    end
                end
                ST_DRAIN: if (m_axi_rvalid && m_axi_rlast) begin
                    state <= ST_IDLE;
                end
                ST_HOLD: if (completion_ack) begin
                    state <= ST_IDLE;
                end
                default: begin end
            endcase
        end
    end
endmodule
