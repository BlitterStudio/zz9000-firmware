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
 * source through unchanged. Subtracts, multiplies and adds sit in separate
 * stages for the 150 MHz pixel clock.
 *
 * Timing: the geometry inputs arrive with the line-buffer read addresses;
 * the words arrive READ_LATENCY cycles later; yuv422 is valid eight cycles
 * after the words.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
module video_overlay_filter #(
  parameter READ_LATENCY = 2
)(
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

/* lerp(a, b, w) runs as b - a, then (b - a) * w, then
 * a + round(product / 256), one register stage each. */
function signed [8:0] lerp_diff;
  input [7:0] a;
  input [7:0] b;
  begin
    lerp_diff = $signed({1'b0, b}) - $signed({1'b0, a});
  end
endfunction

function signed [17:0] lerp_product;
  input signed [8:0] diff;
  input [7:0] w;
  begin
    lerp_product = diff * $signed({1'b0, w});
  end
endfunction

function [7:0] lerp_sum;
  input [7:0] a;
  input signed [17:0] product;
  reg signed [17:0] rounded;
  begin
    rounded = (product + 18'sd128) >>> 8;
    lerp_sum = a + rounded[7:0];
  end
endfunction

/* Geometry delayed to line up with the words. */
localparam GEOMETRY_BITS = 28;
reg [GEOMETRY_BITS-1:0] geometry_delay [0:READ_LATENCY-1];
integer d;
initial
  for (d = 0; d < READ_LATENCY; d = d + 1)
    geometry_delay[d] = 0;

always @(posedge clk) begin
  geometry_delay[0] <= {luma_odd, luma_edge, luma_weight, word_odd,
                        word_edge, chroma_weight, row_weight};
  for (d = 1; d < READ_LATENCY; d = d + 1)
    geometry_delay[d] <= geometry_delay[d - 1];
end

wire a_luma_odd;
wire a_luma_edge;
wire [7:0] a_luma_weight;
wire a_word_odd;
wire a_word_edge;
wire [7:0] a_chroma_weight;
wire [7:0] a_row_weight;
assign {a_luma_odd, a_luma_edge, a_luma_weight, a_word_odd, a_word_edge,
        a_chroma_weight, a_row_weight} = geometry_delay[READ_LATENCY - 1];

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

/* Samples are indexed component * 2 + row: Y, U, V; top row 0, bottom 1. */
localparam SAMPLES = 6;

/* Stage 1: neighbour pairs (a, b) of each sample. */
reg [7:0] s1_a [0:SAMPLES-1];
reg [7:0] s1_b [0:SAMPLES-1];
reg [7:0] s1_luma_weight = 0;
reg [7:0] s1_chroma_weight = 0;
reg [7:0] s1_row_weight = 0;

/* Stage 2: horizontal differences. */
reg [7:0] s2_a [0:SAMPLES-1];
reg signed [8:0] s2_diff [0:SAMPLES-1];
reg [7:0] s2_luma_weight = 0;
reg [7:0] s2_chroma_weight = 0;
reg [7:0] s2_row_weight = 0;

/* Stage 3: horizontal products. */
reg [7:0] s3_a [0:SAMPLES-1];
reg signed [17:0] s3_product [0:SAMPLES-1];
reg [7:0] s3_row_weight = 0;

/* Stage 4 separates the product register from the horizontal lerp. */
reg [7:0] s4_a [0:SAMPLES-1];
reg signed [17:0] s4_product [0:SAMPLES-1];
reg [7:0] s4_row_weight_pre = 0;

/* Stage 5: horizontally filtered rows. */
reg [7:0] s5_h [0:SAMPLES-1];
reg [7:0] s5_row_weight = 0;

/* Stages 6-8: the vertical lerp per component. */
reg [7:0] s6_top [0:2];
reg signed [8:0] s6_diff [0:2];
reg [7:0] s6_row_weight = 0;
reg [7:0] s7_top [0:2];
reg signed [17:0] s7_product [0:2];
reg [7:0] s8_out [0:2];

integer r;
initial begin
  for (r = 0; r < SAMPLES; r = r + 1) begin
    s1_a[r] = 0; s1_b[r] = 0;
    s2_a[r] = 0; s2_diff[r] = 0;
    s3_a[r] = 0; s3_product[r] = 0;
    s4_a[r] = 0; s4_product[r] = 0;
    s5_h[r] = 0;
  end
  for (r = 0; r < 3; r = r + 1) begin
    s6_top[r] = 0; s6_diff[r] = 0;
    s7_top[r] = 0; s7_product[r] = 0;
    s8_out[r] = 0;
  end
end

always @(posedge clk) begin
  s1_a[0] <= top_y0;              s1_b[0] <= top_y1;
  s1_a[1] <= bottom_y0;           s1_b[1] <= bottom_y1;
  s1_a[2] <= top_k_s[15:8];       s1_b[2] <= top_k1_s[15:8];
  s1_a[3] <= bottom_k_s[15:8];    s1_b[3] <= bottom_k1_s[15:8];
  s1_a[4] <= top_k_s[7:0];        s1_b[4] <= top_k1_s[7:0];
  s1_a[5] <= bottom_k_s[7:0];     s1_b[5] <= bottom_k1_s[7:0];
  s1_luma_weight <= a_luma_weight;
  s1_chroma_weight <= a_chroma_weight;
  s1_row_weight <= a_row_weight;

  for (r = 0; r < SAMPLES; r = r + 1) begin
    s2_a[r] <= s1_a[r];
    s2_diff[r] <= lerp_diff(s1_a[r], s1_b[r]);
  end
  s2_luma_weight <= s1_luma_weight;
  s2_chroma_weight <= s1_chroma_weight;
  s2_row_weight <= s1_row_weight;

  for (r = 0; r < SAMPLES; r = r + 1) begin
    s3_a[r] <= s2_a[r];
    s3_product[r] <= lerp_product(s2_diff[r],
                                  r < 2 ? s2_luma_weight : s2_chroma_weight);
  end
  s3_row_weight <= s2_row_weight;

  for (r = 0; r < SAMPLES; r = r + 1) begin
    s4_a[r] <= s3_a[r];
    s4_product[r] <= s3_product[r];
  end
  s4_row_weight_pre <= s3_row_weight;

  for (r = 0; r < SAMPLES; r = r + 1)
    s5_h[r] <= lerp_sum(s4_a[r], s4_product[r]);
  s5_row_weight <= s4_row_weight_pre;

  for (r = 0; r < 3; r = r + 1) begin
    s6_top[r] <= s5_h[2 * r];
    s6_diff[r] <= lerp_diff(s5_h[2 * r], s5_h[2 * r + 1]);
  end
  s6_row_weight <= s5_row_weight;

  for (r = 0; r < 3; r = r + 1) begin
    s7_top[r] <= s6_top[r];
    s7_product[r] <= lerp_product(s6_diff[r], s6_row_weight);
  end

  for (r = 0; r < 3; r = r + 1)
    s8_out[r] <= lerp_sum(s7_top[r], s7_product[r]);
end

assign yuv422 = {s8_out[2], s8_out[0], s8_out[1], s8_out[0]};

endmodule
