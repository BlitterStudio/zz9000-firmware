`timescale 1ns / 1ps
/*
 * One-pixel/clock packed-YUV422 overlay converter/keyer.
 *
 * P96 remains the public overlay API. A line-fetch frontend supplies the
 * selected source macropixel and luma phase; this block performs the existing
 * CCIR601 conversion and RGB color-key decision in the pixel clock domain.
 * Four registered stages (decode, multiply, add, clamp) keep the path viable
 * when the runtime-reprogrammed pixel clock reaches 150 MHz. The Vivado
 * project's clock wizard is statically constrained at its 75 MHz default, so
 * the extra add stage is intentional headroom rather than optional latency.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
module video_overlay_pixel(
  input         clk,
  input         resetn,
  input  [23:0] base_rgb,
  input  [23:0] key_rgb,
  input         key_enable,
  input         overlay_enable,
  input  [31:0] yuv422,
  input  [2:0]  variant,
  input         luma_phase,
  output reg [23:0] out_rgb,
  output reg        out_overlay
);

reg signed [9:0] s1_c, s1_d, s1_e;
reg [23:0] s1_base;
reg s1_draw;

reg signed [19:0] s2_cy, s2_rc, s2_gd, s2_ge, s2_bc;
reg [23:0] s2_base;
reg s2_draw;

reg signed [20:0] s3_r, s3_g, s3_b;
reg [23:0] s3_base;
reg s3_draw;

reg [7:0] y_byte, u_byte, v_byte;

function [7:0] clamp8;
  input signed [20:0] value;
  begin
    if (value < 0)
      clamp8 = 8'd0;
    else if (value > 255)
      clamp8 = 8'd255;
    else
      clamp8 = value[7:0];
  end
endfunction

always @* begin
  /* Byte layouts match enum yuv422_variant in gfx.h. */
  case (variant)
    3'd0: begin /* CGX: Y0 U Y1 V */
      y_byte = luma_phase ? yuv422[23:16] : yuv422[7:0];
      u_byte = yuv422[15:8];
      v_byte = yuv422[31:24];
    end
    3'd1: begin /* STD: Y1 V Y0 U */
      y_byte = luma_phase ? yuv422[7:0] : yuv422[23:16];
      u_byte = yuv422[31:24];
      v_byte = yuv422[15:8];
    end
    3'd2: begin /* PC: U Y0 V Y1 */
      y_byte = luma_phase ? yuv422[31:24] : yuv422[15:8];
      u_byte = yuv422[7:0];
      v_byte = yuv422[23:16];
    end
    3'd3: begin /* PA: Y0 Y1 U V */
      y_byte = luma_phase ? yuv422[15:8] : yuv422[7:0];
      u_byte = yuv422[23:16];
      v_byte = yuv422[31:24];
    end
    default: begin /* PAPC: V U Y1 Y0 */
      y_byte = luma_phase ? yuv422[23:16] : yuv422[31:24];
      u_byte = yuv422[15:8];
      v_byte = yuv422[7:0];
    end
  endcase
end

always @(posedge clk) begin
  if (!resetn) begin
    s1_c <= 0; s1_d <= 0; s1_e <= 0;
    s1_base <= 0; s1_draw <= 0;
    s2_cy <= 0; s2_rc <= 0; s2_gd <= 0; s2_ge <= 0; s2_bc <= 0;
    s2_base <= 0; s2_draw <= 0;
    s3_r <= 0; s3_g <= 0; s3_b <= 0;
    s3_base <= 0; s3_draw <= 0;
    out_rgb <= 0; out_overlay <= 0;
  end else begin
    s1_c <= $signed({1'b0, y_byte}) - 10'sd16;
    s1_d <= $signed({1'b0, u_byte}) - 10'sd128;
    s1_e <= $signed({1'b0, v_byte}) - 10'sd128;
    s1_base <= base_rgb;
    s1_draw <= overlay_enable && (!key_enable || base_rgb == key_rgb);

    s2_cy <= 20'sd298 * s1_c;
    s2_rc <= 20'sd409 * s1_e;
    s2_gd <= -20'sd100 * s1_d;
    s2_ge <= -20'sd208 * s1_e;
    s2_bc <= 20'sd516 * s1_d;
    s2_base <= s1_base;
    s2_draw <= s1_draw;

    s3_r <= $signed(s2_cy) + $signed(s2_rc) + 21'sd128;
    s3_g <= $signed(s2_cy) + $signed(s2_gd) + $signed(s2_ge) + 21'sd128;
    s3_b <= $signed(s2_cy) + $signed(s2_bc) + 21'sd128;
    s3_base <= s2_base;
    s3_draw <= s2_draw;

    if (s3_draw) begin
      out_rgb[23:16] <= clamp8($signed(s3_r) >>> 8);
      out_rgb[15:8]  <= clamp8($signed(s3_g) >>> 8);
      out_rgb[7:0]   <= clamp8($signed(s3_b) >>> 8);
      out_overlay <= 1'b1;
    end else begin
      out_rgb <= s3_base;
      out_overlay <= 1'b0;
    end
  end
end

endmodule
