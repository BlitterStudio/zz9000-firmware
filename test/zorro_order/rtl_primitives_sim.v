`timescale 1ns/1ps
/* Minimal behavioral stand-ins for primitives outside the ordering path. */
module IOBUF(input I, input T, inout IO, output O);
  assign IO = T ? 1'bz : I;
  assign O = IO;
endmodule

module ODDR #(
  parameter DDR_CLK_EDGE = "OPPOSITE_EDGE",
  parameter INIT = 1'b0,
  parameter SRTYPE = "ASYNC"
) (
  output reg Q = INIT, input C, input CE, input D1, input D2, input R, input S
);
  always @(posedge C or posedge R or posedge S)
    if (R) Q <= 1'b0;
    else if (S) Q <= 1'b1;
    else if (CE) Q <= D1;
  always @(negedge C) if (CE && !R && !S) Q <= D2;
endmodule

module xpm_cdc_array_single #(
  parameter integer DEST_SYNC_FF = 3,
  parameter integer INIT_SYNC_FF = 1,
  parameter integer SIM_ASSERT_CHK = 0,
  parameter integer SRC_INPUT_REG = 0,
  parameter integer WIDTH = 1
) (
  input wire src_clk, input wire [WIDTH-1:0] src_in,
  input wire dest_clk, output wire [WIDTH-1:0] dest_out
);
  reg [WIDTH-1:0] stages [0:DEST_SYNC_FF-1];
  integer i;
  initial for (i = 0; i < DEST_SYNC_FF; i = i + 1) stages[i] = 0;
  always @(posedge dest_clk) begin
    stages[0] <= src_in;
    for (i = 1; i < DEST_SYNC_FF; i = i + 1) stages[i] <= stages[i-1];
  end
  assign dest_out = stages[DEST_SYNC_FF-1];
endmodule

module MMCME2_ADV #(
  parameter BANDWIDTH = "OPTIMIZED",
  parameter real CLKFBOUT_MULT_F = 5.0,
  parameter real CLKFBOUT_PHASE = 0.0,
  parameter CLKFBOUT_USE_FINE_PS = "FALSE",
  parameter real CLKIN1_PERIOD = 0.0,
  parameter real CLKIN2_PERIOD = 0.0,
  parameter real CLKOUT0_DIVIDE_F = 1.0,
  parameter real CLKOUT0_DUTY_CYCLE = 0.5,
  parameter real CLKOUT0_PHASE = 0.0,
  parameter CLKOUT0_USE_FINE_PS = "FALSE",
  parameter integer CLKOUT1_DIVIDE = 1,
  parameter real CLKOUT1_DUTY_CYCLE = 0.5,
  parameter real CLKOUT1_PHASE = 0.0,
  parameter CLKOUT1_USE_FINE_PS = "FALSE",
  parameter COMPENSATION = "ZHOLD",
  parameter integer DIVCLK_DIVIDE = 1,
  parameter IS_CLKINSEL_INVERTED = 1'b0,
  parameter IS_PSEN_INVERTED = 1'b0,
  parameter IS_PSINCDEC_INVERTED = 1'b0,
  parameter IS_PWRDWN_INVERTED = 1'b0,
  parameter IS_RST_INVERTED = 1'b0,
  parameter real REF_JITTER1 = 0.0,
  parameter real REF_JITTER2 = 0.0,
  parameter SS_EN = "FALSE",
  parameter SS_MODE = "CENTER_HIGH",
  parameter integer SS_MOD_PERIOD = 10000,
  parameter STARTUP_WAIT = "FALSE"
) (
  input CLKFBIN, output CLKFBOUT, input CLKIN1, input CLKIN2, input CLKINSEL,
  output CLKOUT0, output CLKOUT1, input [6:0] DADDR, input DCLK, input DEN,
  input [15:0] DI, input DWE, output LOCKED, input PSCLK, output PSDONE,
  input PSEN, input PSINCDEC, input PWRDWN, input RST
);
  assign CLKFBOUT = 1'b0;
  assign CLKOUT0 = CLKIN1;
  assign CLKOUT1 = CLKIN1;
  assign LOCKED = !RST;
  assign PSDONE = 1'b0;
endmodule
