// Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
`timescale 1ns / 1ps

//
// Last stage of the SiI9022A parallel-video interface.  All data and control
// signals get one identical pixel-clock register before the FPGA I/O cells.
// VGA_PCLK is a clock-forwarding ODDR: data launch on the rising edge and the
// SiI9022A's EDGE=0 falling-edge capture are separated by half a pixel period.

(* KEEP_HIERARCHY = "yes" *)
module vga_output_stage(
  input pixel_clk,
  input [7:0] rgb_r_in,
  input [7:0] rgb_g_in,
  input [7:0] rgb_b_in,
  input hsync_in,
  input vsync_in,
  input de_in,
  (* IOB = "TRUE" *) output reg [7:0] VGA_R,
  (* IOB = "TRUE" *) output reg [7:0] VGA_G,
  (* IOB = "TRUE" *) output reg [7:0] VGA_B,
  (* IOB = "TRUE" *) output reg VGA_HS,
  (* IOB = "TRUE" *) output reg VGA_VS,
  (* IOB = "TRUE" *) output reg VGA_DE,
  output VGA_PCLK
);

always @(posedge pixel_clk) begin
  VGA_R <= rgb_r_in;
  VGA_G <= rgb_g_in;
  VGA_B <= rgb_b_in;
  VGA_HS <= hsync_in;
  VGA_VS <= vsync_in;
  VGA_DE <= de_in;
end


// D1=1/D2=0 forwards a non-inverted pixel clock.  Firmware programs the
// SiI9022A Edge Select bit clear, so it samples these registered outputs on
// the falling edge (SiI-DS-1076-E.01 Table 3.13 and Figure 4.2).
(* DONT_TOUCH = "TRUE" *) ODDR #(
  .DDR_CLK_EDGE("SAME_EDGE"),
  .INIT(1'b0),
  .SRTYPE("SYNC")
) vga_pclk_oddr (
  .C(pixel_clk),
  .CE(1'b1),
  .D1(1'b1),
  .D2(1'b0),
  .Q(VGA_PCLK),
  .R(1'b0),
  .S(1'b0)
);

endmodule
