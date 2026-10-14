/*
 * Bit-exact reference for the native overlay's bilinear scaler
 * (video_formatter.v position steppers + video_overlay_filter.v +
 * video_overlay_pixel.v). The including bench defines the source samples:
 *   function integer ovl_ref_y(input integer line, input integer pixel);
 *   function integer ovl_ref_u(input integer line, input integer word);
 *   function integer ovl_ref_v(input integer line, input integer word);
 *
 * Output pixel d of a dst-wide window samples the 16.16 position
 * d * step + step / 2 - 0.5 with step = (src << 16) / dst; chroma uses half
 * that position over the macropixel words. Neighbours clamp at the last
 * sample with weight zero.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

function integer ovl_lerp(input integer a, input integer b, input integer w);
  ovl_lerp = a + (((b - a) * w + 128) >>> 8);
endfunction

/* Returns {index[11:0], next index[11:0], weight[7:0]}. */
function [31:0] ovl_axis(input integer d, input integer src,
                         input integer dst, input integer last,
                         input integer chroma);
  integer step, pos, fl, fl1, w;
  begin
    step = (src * 65536) / dst;
    pos = d * step + (step >>> 1) - 32768;
    if (chroma)
      pos = pos >>> 1;
    fl = pos < 0 ? 0 : (pos >>> 16);
    if (fl >= last) begin
      ovl_axis = {last[11:0], last[11:0], 8'd0};
    end else begin
      w = pos < 0 ? 0 : ((pos >>> 8) & 255);
      fl1 = fl + 1;
      ovl_axis = {fl[11:0], fl1[11:0], w[7:0]};
    end
  end
endfunction

function [7:0] ovl_clamp8(input integer value);
  ovl_clamp8 = value < 0 ? 8'd0 : (value > 255 ? 8'd255 : value[7:0]);
endfunction

/* dvi_rgb of window pixel (d, e) for a src_w x src_h source shown at
 * dst_w x dst_h. */
function [31:0] ovl_expected(input integer d, input integer e,
                             input integer src_w, input integer src_h,
                             input integer dst_w, input integer dst_h);
  reg [31:0] ax, ac, ay;
  integer i0, i1, wx, k0, k1, wc, r0, r1, wy;
  integer y, u, v, c, cd, ce;
  begin
    ax = ovl_axis(d, src_w, dst_w, src_w - 1, 0);
    ac = ovl_axis(d, src_w, dst_w, (src_w + 1) / 2 - 1, 1);
    ay = ovl_axis(e, src_h, dst_h, src_h - 1, 0);
    i0 = ax[31:20]; i1 = ax[19:8]; wx = ax[7:0];
    k0 = ac[31:20]; k1 = ac[19:8]; wc = ac[7:0];
    r0 = ay[31:20]; r1 = ay[19:8]; wy = ay[7:0];
    y = ovl_lerp(ovl_lerp(ovl_ref_y(r0, i0), ovl_ref_y(r0, i1), wx),
                 ovl_lerp(ovl_ref_y(r1, i0), ovl_ref_y(r1, i1), wx), wy);
    u = ovl_lerp(ovl_lerp(ovl_ref_u(r0, k0), ovl_ref_u(r0, k1), wc),
                 ovl_lerp(ovl_ref_u(r1, k0), ovl_ref_u(r1, k1), wc), wy);
    v = ovl_lerp(ovl_lerp(ovl_ref_v(r0, k0), ovl_ref_v(r0, k1), wc),
                 ovl_lerp(ovl_ref_v(r1, k0), ovl_ref_v(r1, k1), wc), wy);
    c = y - 16;
    cd = u - 128;
    ce = v - 128;
    ovl_expected = {8'h00,
                    ovl_clamp8((298 * c + 409 * ce + 128) >>> 8),
                    ovl_clamp8((298 * c - 100 * cd - 208 * ce + 128) >>> 8),
                    ovl_clamp8((298 * c + 516 * cd + 128) >>> 8)};
  end
endfunction
