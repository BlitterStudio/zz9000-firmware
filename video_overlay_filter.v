`timescale 1ns / 1ps
/*
 * Bilinear filter for the native P96 overlay.
 *
 * The line buffer returns, for two adjacent source rows, the even and odd
 * macropixel words that hold luma pixels i and i+1 and chroma macropixels
 * k and k+1. This block picks the samples for the stored YUV422 layout,
 * blends them horizontally with the luma and chroma weights, then
 * vertically with the row weight, and emits one CGX word (luma in [7:0])
 * for video_overlay_pixel.
 *
 * lerp(a, b, w) = a + (((b - a) * w + 128) >>> 8), with w in 1/256 steps.
 * A weight of zero returns a exactly, so an unscaled window passes the
 * source through unchanged. Multiplies and adds sit in separate stages for
 * the 150 MHz pixel clock.
 *
 * Timing: the geometry inputs arrive with the line-buffer read addresses;
 * the words arrive one cycle later; yuv422 is valid OVERLAY_FILTER_DELAY (5)
 * cycles after the words.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
module video_overlay_filter(
  input         clk,
  input  [2:0]  variant,
  input         luma_odd,
  input         luma_edge,
  input  [7:0]  luma_weight,
  input         word_odd,
  input         word_edge,
  input  [7:0]  chroma_weight,
  input  [7:0]  row_weight,
  input  [31:0] top_even,
  input  [31:0] top_odd,
  input  [31:0] bottom_even,
  input  [31:0] bottom_odd,
  output [31:0] yuv422
);

/* Byte layouts match enum yuv422_variant in gfx.h and video_overlay_pixel.
 * Returns {Y0, Y1, U, V}. */
function [31:0] unpack;
  input [2:0] layout;
  input [31:0] w;
  begin
    case (layout)
      3'd0: unpack = {w[7:0], w[23:16], w[15:8], w[31:24]};   /* Y0 U Y1 V */
      3'd1: unpack = {w[23:16], w[7:0], w[31:24], w[15:8]};   /* Y1 V Y0 U */
      3'd2: unpack = {w[15:8], w[31:24], w[7:0], w[23:16]};   /* U Y0 V Y1 */
      3'd3: unpack = {w[7:0], w[15:8], w[23:16], w[31:24]};   /* Y0 Y1 U V */
      default: unpack = {w[31:24], w[23:16], w[15:8], w[7:0]}; /* V U Y1 Y0 */
    endcase
  end
endfunction

/* (b - a) * w, the multiply half of lerp. */
function signed [17:0] lerp_product;
  input [7:0] a;
  input [7:0] b;
  input [7:0] w;
  begin
    lerp_product = ($signed({1'b0, b}) - $signed({1'b0, a})) *
                   $signed({1'b0, w});
  end
endfunction

/* a + round(product / 256), the add half of lerp. */
function [7:0] lerp_sum;
  input [7:0] a;
  input signed [17:0] product;
  reg signed [17:0] rounded;
  begin
    rounded = (product + 18'sd128) >>> 8;
    lerp_sum = a + rounded[7:0];
  end
endfunction

/* Geometry aligned with the words. */
reg a_luma_odd = 0;
reg a_luma_edge = 0;
reg [7:0] a_luma_weight = 0;
reg a_word_odd = 0;
reg a_word_edge = 0;
reg [7:0] a_chroma_weight = 0;
reg [7:0] a_row_weight = 0;

always @(posedge clk) begin
  a_luma_odd <= luma_odd;
  a_luma_edge <= luma_edge;
  a_luma_weight <= luma_weight;
  a_word_odd <= word_odd;
  a_word_edge <= word_edge;
  a_chroma_weight <= chroma_weight;
  a_row_weight <= row_weight;
end

/* Words k and k+1 of each row; k+1 is k at the right edge. */
wire [31:0] top_k = a_word_odd ? top_odd : top_even;
wire [31:0] top_k1 = a_word_edge ? top_k : (a_word_odd ? top_even : top_odd);
wire [31:0] bottom_k = a_word_odd ? bottom_odd : bottom_even;
wire [31:0] bottom_k1 =
  a_word_edge ? bottom_k : (a_word_odd ? bottom_even : bottom_odd);
wire [31:0] top_k_s = unpack(variant, top_k);
wire [31:0] top_k1_s = unpack(variant, top_k1);
wire [31:0] bottom_k_s = unpack(variant, bottom_k);
wire [31:0] bottom_k1_s = unpack(variant, bottom_k1);

/* Luma i lies in word k; i+1 is the other luma of word k when i is even,
 * the first luma of word k+1 when i is odd, and i itself at the edge. */
wire [7:0] top_y0 = a_luma_odd ? top_k_s[23:16] : top_k_s[31:24];
wire [7:0] top_y1 = a_luma_edge ? top_y0 :
                    (a_luma_odd ? top_k1_s[31:24] : top_k_s[23:16]);
wire [7:0] bottom_y0 = a_luma_odd ? bottom_k_s[23:16] : bottom_k_s[31:24];
wire [7:0] bottom_y1 = a_luma_edge ? bottom_y0 :
                       (a_luma_odd ? bottom_k1_s[31:24] : bottom_k_s[23:16]);

/* Stage 1: samples. Index 0 top row, 1 bottom row. */
reg [7:0] s1_y0 [0:1];
reg [7:0] s1_y1 [0:1];
reg [7:0] s1_u0 [0:1];
reg [7:0] s1_u1 [0:1];
reg [7:0] s1_v0 [0:1];
reg [7:0] s1_v1 [0:1];
reg [7:0] s1_luma_weight = 0;
reg [7:0] s1_chroma_weight = 0;
reg [7:0] s1_row_weight = 0;

/* Stage 2: horizontal products. */
reg [7:0] s2_y [0:1];
reg [7:0] s2_u [0:1];
reg [7:0] s2_v [0:1];
reg signed [17:0] s2_yp [0:1];
reg signed [17:0] s2_up [0:1];
reg signed [17:0] s2_vp [0:1];
reg [7:0] s2_row_weight = 0;

/* Stage 3: horizontally filtered rows. */
reg [7:0] s3_y [0:1];
reg [7:0] s3_u [0:1];
reg [7:0] s3_v [0:1];
reg [7:0] s3_row_weight = 0;

/* Stage 4: vertical products. */
reg [7:0] s4_y = 0, s4_u = 0, s4_v = 0;
reg signed [17:0] s4_yp = 0, s4_up = 0, s4_vp = 0;

/* Stage 5: output. */
reg [7:0] s5_y = 0, s5_u = 0, s5_v = 0;

integer r;
initial begin
  for (r = 0; r < 2; r = r + 1) begin
    s1_y0[r] = 0; s1_y1[r] = 0; s1_u0[r] = 0; s1_u1[r] = 0;
    s1_v0[r] = 0; s1_v1[r] = 0;
    s2_y[r] = 0; s2_u[r] = 0; s2_v[r] = 0;
    s2_yp[r] = 0; s2_up[r] = 0; s2_vp[r] = 0;
    s3_y[r] = 0; s3_u[r] = 0; s3_v[r] = 0;
  end
end

always @(posedge clk) begin
  s1_y0[0] <= top_y0;
  s1_y1[0] <= top_y1;
  s1_u0[0] <= top_k_s[15:8];
  s1_u1[0] <= top_k1_s[15:8];
  s1_v0[0] <= top_k_s[7:0];
  s1_v1[0] <= top_k1_s[7:0];
  s1_y0[1] <= bottom_y0;
  s1_y1[1] <= bottom_y1;
  s1_u0[1] <= bottom_k_s[15:8];
  s1_u1[1] <= bottom_k1_s[15:8];
  s1_v0[1] <= bottom_k_s[7:0];
  s1_v1[1] <= bottom_k1_s[7:0];
  s1_luma_weight <= a_luma_weight;
  s1_chroma_weight <= a_chroma_weight;
  s1_row_weight <= a_row_weight;

  for (r = 0; r < 2; r = r + 1) begin
    s2_y[r] <= s1_y0[r];
    s2_u[r] <= s1_u0[r];
    s2_v[r] <= s1_v0[r];
    s2_yp[r] <= lerp_product(s1_y0[r], s1_y1[r], s1_luma_weight);
    s2_up[r] <= lerp_product(s1_u0[r], s1_u1[r], s1_chroma_weight);
    s2_vp[r] <= lerp_product(s1_v0[r], s1_v1[r], s1_chroma_weight);
  end
  s2_row_weight <= s1_row_weight;

  for (r = 0; r < 2; r = r + 1) begin
    s3_y[r] <= lerp_sum(s2_y[r], s2_yp[r]);
    s3_u[r] <= lerp_sum(s2_u[r], s2_up[r]);
    s3_v[r] <= lerp_sum(s2_v[r], s2_vp[r]);
  end
  s3_row_weight <= s2_row_weight;

  s4_y <= s3_y[0];
  s4_u <= s3_u[0];
  s4_v <= s3_v[0];
  s4_yp <= lerp_product(s3_y[0], s3_y[1], s3_row_weight);
  s4_up <= lerp_product(s3_u[0], s3_u[1], s3_row_weight);
  s4_vp <= lerp_product(s3_v[0], s3_v[1], s3_row_weight);

  s5_y <= lerp_sum(s4_y, s4_yp);
  s5_u <= lerp_sum(s4_u, s4_up);
  s5_v <= lerp_sum(s4_v, s4_vp);
end

assign yuv422 = {s5_v, s5_y, s5_u, s5_y};

endmodule
