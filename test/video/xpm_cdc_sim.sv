`timescale 1ns / 1ps

/* Test-only synchronizer/handshake behavior for the XPM CDC instances used by
 * videocap_sampler. This does not model metastability or FPGA timing; Vivado
 * xsim and the routed timing audits remain required for those properties. */
module xpm_cdc_single #(
    parameter integer DEST_SYNC_FF = 3,
    parameter integer INIT_SYNC_FF = 1,
    parameter integer SIM_ASSERT_CHK = 0,
    parameter integer SRC_INPUT_REG = 0,
    parameter integer WIDTH = 1
) (
    input wire src_clk, input wire [WIDTH-1:0] src_in,
    input wire dest_clk, output wire [WIDTH-1:0] dest_out
);
    reg [WIDTH-1:0] source_value = 0;
    reg [WIDTH-1:0] stages [0:DEST_SYNC_FF-1];
    integer i;
    initial for (i = 0; i < DEST_SYNC_FF; i = i + 1) stages[i] = 0;
    always @(posedge src_clk) source_value <= src_in;
    always @(posedge dest_clk) begin
        stages[0] <= SRC_INPUT_REG ? source_value : src_in;
        for (integer j = 1; j < DEST_SYNC_FF; j = j + 1)
            stages[j] <= stages[j-1];
    end
    assign dest_out = stages[DEST_SYNC_FF-1];
endmodule

module xpm_cdc_handshake #(
    parameter integer DEST_EXT_HSK = 0,
    parameter integer DEST_SYNC_FF = 4,
    parameter integer INIT_SYNC_FF = 1,
    parameter integer SIM_ASSERT_CHK = 0,
    parameter integer SRC_SYNC_FF = 4,
    parameter integer WIDTH = 1
) (
    input wire src_clk, input wire [WIDTH-1:0] src_in,
    input wire src_send, output wire src_rcv,
    input wire dest_clk, output reg [WIDTH-1:0] dest_out = 0,
    output wire dest_req, input wire dest_ack
);
    reg [WIDTH-1:0] held_payload = 0;
    reg [DEST_SYNC_FF-1:0] send_stages = 0;
    reg [SRC_SYNC_FF-1:0] ack_stages = 0;
    reg source_sending = 0;
    reg destination_request = 0;
    wire acknowledged = DEST_EXT_HSK ? dest_ack : destination_request;

    always @(posedge src_clk) begin
        if (src_send && !source_sending) held_payload <= src_in;
        source_sending <= src_send;
        ack_stages <= {ack_stages[SRC_SYNC_FF-2:0], acknowledged};
    end
    always @(posedge dest_clk) begin
        send_stages <= {send_stages[DEST_SYNC_FF-2:0], src_send};
        if (send_stages[DEST_SYNC_FF-1] && !destination_request)
            dest_out <= held_payload;
        destination_request <= send_stages[DEST_SYNC_FF-1];
    end
    assign src_rcv = ack_stages[SRC_SYNC_FF-1];
    assign dest_req = destination_request;
endmodule
