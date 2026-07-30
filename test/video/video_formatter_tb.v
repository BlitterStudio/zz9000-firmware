`timescale 1ns / 1ps
/*
 * Functional testbench for video_formatter.v
 *
 * Models the axi_vdma MM2S stream (64-bit beats, tkeep, tuser frame start,
 * tlast per line), programs the formatter control ops the same way the
 * firmware does, captures dvi_rgb during active video and compares every
 * pixel of a settled frame against the expected framebuffer contents.
 *
 * Plusargs:
 *   +CMODE=<0|1|2|3>   colormode (8/16/32/15 bit), default 2
 *   +SCALEX=<0|1>      horizontal doubling, default 0
 *   +SCALEY=<0|1>      vertical doubling (native videocap uses 1), default 0
 *   +WIDTH=<pixels>    displayed width, default 64
 *
 * Compile with -d MASTER_DUT to run against the pre-branch 32-bit formatter
 * (reference model validation).
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

module video_formatter_tb;

localparam NLINES = 8;      // active lines (v_rez)
localparam V_MAX = 16;
localparam VS_START = 10;
localparam VS_END = 12;
localparam MAXW = 1920;

localparam OP_COLORMODE = 1;
localparam OP_DIMENSIONS = 2;
localparam OP_PALETTE = 3;
localparam OP_SCALE = 4;
localparam OP_MAX = 6;
localparam OP_HS = 7;
localparam OP_VS = 8;
localparam OP_DPMS = 21;
localparam OP_SPRITEXY = 13;
localparam OP_OVERLAY_CTRL = 22;
localparam OP_OVERLAY_POS = 23;
localparam OP_OVERLAY_SIZE = 24;
localparam OP_OVERLAY_KEY = 25;
localparam OP_OVERLAY_SOURCE_SIZE = 26;
localparam OP_OVERLAY_FRAME = 27;

// config (from plusargs)
integer cfg_cmode;
integer cfg_scalex;
integer cfg_scaley;
integer cfg_width;
integer words_per_line;
integer beats_per_line;
integer src_pixels;
integer src_lines;
integer h_max;

// clocks
reg aclk = 0;
reg dvi_clk = 0;
reg dvi_ena = 0;
always #5 aclk = ~aclk;               // 100 MHz
always #3.4 dvi_clk = dvi_ena ? ~dvi_clk : 1'b0; // ~147 MHz

// dut i/o
reg [63:0] tdata = 0;
reg [7:0] tkeep = 0;
reg tlast = 0;
reg [0:0] tuser = 0;
reg tvalid = 0;
wire tready;
reg aresetn = 0;
wire dvi_hsync, dvi_vsync, dvi_active_video;
wire [31:0] dvi_rgb;
reg [31:0] control_data = 0;
reg [7:0] control_op = 0;
wire [1:0] control_vblank;
reg [31:0] overlay_tdata = 0;
reg [3:0] overlay_tkeep = 4'hf;
reg overlay_tlast = 0;
reg overlay_tuser = 0;
reg overlay_tvalid = 0;
wire overlay_tready;

`ifdef MASTER_DUT
video_formatter uut (
  .m_axis_vid_tdata(tdata[31:0]),
`else
video_formatter uut (
  .m_axis_vid_tdata(tdata),
  .m_axis_vid_tkeep(tkeep),
  .overlay_axis_tdata(overlay_tdata),
  .overlay_axis_tkeep(overlay_tkeep),
  .overlay_axis_tlast(overlay_tlast),
  .overlay_axis_tready(overlay_tready),
  .overlay_axis_tuser(overlay_tuser),
  .overlay_axis_tvalid(overlay_tvalid),
`endif
  .m_axis_vid_tlast(tlast),
  .m_axis_vid_tready(tready),
  .m_axis_vid_tuser(tuser),
  .m_axis_vid_tvalid(tvalid),
  .m_axis_vid_aclk(aclk),
  .aresetn(aresetn),
  .dvi_clk(dvi_clk),
  .dvi_hsync(dvi_hsync),
  .dvi_vsync(dvi_vsync),
  .dvi_active_video(dvi_active_video),
  .dvi_rgb(dvi_rgb),
  .control_data(control_data),
  .control_op(control_op),
  .control_interlace(1'b0),
  .control_vblank(control_vblank),
  .scanline_intensity(8'd0),
  .scanline_width(2'b00),
  .scanline_parity(1'b0),
  .scanline_intensity2(8'd0)
);

// zero the DUT registers that hardware clears via GSR but plain RTL sim
// leaves at X (they would poison the state machines forever)
initial begin
  uut.counter_x = 0;
  uut.counter_y = 0;
  uut.counter_scanout = 0;
  uut.need_line_fetch = 0;
  uut.need_line_fetch_reg = 0;
  uut.need_line_fetch_reg2 = 0;
  uut.last_line_fetch = 0;
  uut.need_frame_sync = 0;
  uut.need_frame_sync_reg = 0;
  uut.vsync_request = 0;
  uut.sprite_x = 2000;
  uut.sprite_y = 2000;
  uut.vga_sprite_x = 2000;
  uut.vga_sprite_y = 2000;
  uut.vga_sprite_x2 = 2032;
  uut.vga_sprite_y2 = 2048;
  uut.sprite_px = 0;
  uut.sprite_py = 0;
  uut.sprite_on = 0;
  uut.dvi_active_video = 0;
end

`ifndef MASTER_DUT
/* Three-line, nine-pixel black overlay.  Five 4-byte macropixels are sent per
 * line so the final (unused) luma exercises odd destination widths. */
reg overlay_stream_en = 0;
reg early_overlay_prefetch_seen = 0;
integer overlay_stream_width = 9;
integer overlay_frame_width;
integer ol, ow;
integer overlay_stream_generation = 1;
integer overlay_pattern_mode = 0;

function [7:0] overlay_luma(input integer line);
  overlay_luma = 8'd16 + line * 8'd64;
endfunction

function [31:0] expected_overlay(input integer line);
  reg [7:0] gray;
  begin
    gray = (298 * (overlay_luma(line) - 16) + 128) >> 8;
    expected_overlay = {8'h00, gray, gray, gray};
  end
endfunction

function [7:0] overlay_pattern_luma(input integer line,
                                    input integer pixel);
  overlay_pattern_luma = 8'd24 + line * 8'd48 + pixel * 8'd4;
endfunction

function [31:0] expected_overlay_pixel(input integer line,
                                       input integer pixel);
  integer gray;
  begin
    gray = (298 * (overlay_pattern_luma(line, pixel) - 16) + 128) >> 8;
    expected_overlay_pixel = {8'h00, gray[7:0], gray[7:0], gray[7:0]};
  end
endfunction

initial begin : overlay_vdma
  wait (overlay_stream_en);
  forever begin
    overlay_frame_width = overlay_stream_width;
    for (ol = 0; ol < 3; ol = ol + 1) begin
      for (ow = 0; ow < (overlay_frame_width + 1) / 2; ow = ow + 1) begin
        @(negedge aclk);
        if (overlay_pattern_mode)
          overlay_tdata <= {
            8'd128, overlay_pattern_luma(ol, 2 * ow + 1),
            8'd128, overlay_pattern_luma(ol, 2 * ow)};
        else
          overlay_tdata <= {8'd128, overlay_luma(ol),
                            8'd128, overlay_luma(ol)};
        overlay_tuser <= (ol == 0 && ow == 0);
        overlay_tlast <= (ow == (overlay_frame_width + 1) / 2 - 1);
        overlay_tvalid <= 1;
        while (overlay_tready !== 1'b1) @(negedge aclk);
        @(posedge aclk);
      end
    end
    /* Model the real ISR ordering: MM2S remains stalled after the final row,
     * then firmware commits/re-arms only after the vblank interrupt. */
    @(negedge aclk);
    overlay_tvalid <= 0;
    overlay_tuser <= 0;
    overlay_tlast <= 0;
    wait (control_vblank[1] === 1'b1);
    overlay_stream_generation = overlay_stream_generation + 1;
    op(OP_OVERLAY_FRAME, overlay_stream_generation);
  end
end

/* The old design could not advance the fetch request until the final PIP
 * pixel because that same register selected the read bank. Require line one
 * to be requested earlier while line zero remains selected for display. */
always @(posedge dvi_clk)
  if (overlay_stream_en && uut.vga_overlay_enable &&
      uut.overlay_displayed_line == 0 && uut.overlay_fetch_line == 1 &&
      uut.overlay_screen_x <
        ($signed(uut.vga_overlay_x) +
         $signed({1'b0, uut.vga_overlay_width}) - 1))
    early_overlay_prefetch_seen <= 1;
`endif

// framebuffer content generator: word w of line l, byte lanes ascend so
// every neighbouring byte and word differs
function [31:0] fb_word(input integer l, input integer w);
  reg [7:0] base;
  begin
    base = (l * 137 + w * 4) & 8'hFF;
    fb_word = {base + 8'd3, base + 8'd2, base + 8'd1, base};
  end
endfunction

function [23:0] pal24(input [7:0] i);
  pal24 = {i, i ^ 8'hA5, ~i};
endfunction

// expected dvi_rgb for displayed pixel x of active row showing line l
function [31:0] expected_pix(input integer l, input integer x);
  integer xs;
  reg [31:0] w;
  reg [15:0] p16;
  reg [7:0] b;
  begin
    xs = x >> cfg_scalex;
    case (cfg_cmode)
      0: begin // 8 bit
        w = fb_word(l, xs >> 2);
        case (xs & 3)
          0: b = w[7:0];
          1: b = w[15:8];
          2: b = w[23:16];
          3: b = w[31:24];
        endcase
        expected_pix = {8'h00, pal24(b)};
      end
      1: begin // 16 bit 565
        w = fb_word(l, xs >> 1);
        p16 = (xs & 1) ? {w[23:16], w[31:24]} : {w[7:0], w[15:8]};
        expected_pix = {8'h00,
                        {p16[15:11], p16[15:13]},  // blue
                        {p16[10:5],  p16[10:9]},   // green
                        {p16[4:0],   p16[4:2]}};   // red
      end
      3: begin // 15 bit 555
        w = fb_word(l, xs >> 1);
        p16 = (xs & 1) ? {w[23:16], w[31:24]} : {w[7:0], w[15:8]};
        expected_pix = {8'h00,
                        {p16[14:10], p16[14:12]},
                        {p16[9:5],   p16[9:7]},
                        {p16[4:0],   p16[4:2]}};
      end
      default: // 32 bit
        expected_pix = fb_word(l, xs);
    endcase
  end
endfunction

// control op helper
task op(input [7:0] o, input [31:0] d);
  begin
    @(negedge aclk);
    control_op <= o;
    control_data <= d;
    @(negedge aclk);
    @(negedge aclk);
    control_op <= 0;
    @(negedge aclk);
  end
endtask

// vdma stream model
reg stream_en = 0;
integer sl, sb, sw0, sw1;
reg s_last;
reg hs_done;
initial begin : vdma
  wait (stream_en);
  forever begin
    for (sl = 0; sl < src_lines; sl = sl + 1) begin
      for (sb = 0; sb < beats_per_line; sb = sb + 1) begin
        sw0 = 2 * sb;
        sw1 = 2 * sb + 1;
        s_last = (sb == beats_per_line - 1);
        @(negedge aclk);
`ifdef MASTER_DUT
        tdata <= {32'hDEADBEEF, fb_word(sl, sb)};
        tkeep <= 8'h0F;
        tuser <= (sl == 0 && sb == 0);
        tlast <= (sb == words_per_line - 1);
`else
        tdata <= {(sw1 < words_per_line) ? fb_word(sl, sw1) : 32'hDEADBEEF,
                  fb_word(sl, sw0)};
        tkeep <= (s_last && (words_per_line & 1)) ? 8'h0F : 8'hFF;
        tuser <= (sl == 0 && sb == 0);
        tlast <= s_last;
`endif
        tvalid <= 1;
        hs_done = 0;
        while (!hs_done) begin
          @(posedge aclk);
          if (tready === 1'b1)
            hs_done = 1;
        end
      end
    end
  end
end

// capture displayed pixels; without scale_y the active rows are
// counter_y = 1..NLINES showing streamed lines 0..NLINES-1 in order, with
// scale_y they are counter_y = 2..NLINES+1 showing each source line twice
reg [31:0] cap [0:(NLINES+1)*MAXW-1];
reg [11:0] xcnt = 0;
integer row;
always @(posedge dvi_clk) begin
  if (dvi_active_video === 1'b1) begin
    row = uut.counter_y - 1;
    if (row >= 0 && row <= NLINES && xcnt < MAXW)
      cap[row * MAXW + xcnt] <= dvi_rgb;
    xcnt <= xcnt + 1;
  end else
    xcnt <= 0;
end

// frame counter (ticks once per frame at vsync start)
integer frames = 0;
always @(posedge dvi_clk)
  if (uut.counter_y == VS_START && uut.counter_x == 0)
    frames = frames + 1;

`ifndef MASTER_DUT
integer dpms_errors = 0;
integer dpms_start_frame;
reg dpms_hsync_seen;
reg dpms_vsync_seen;

task check_dpms(input [1:0] level, input integer expect_hsync,
                input integer expect_vsync);
  begin
    op(OP_DPMS, level);

    // Discard the partial frame containing the clock-domain handoff, then
    // observe a complete frame. A live sync differs from its inactive level.
    dpms_start_frame = frames;
    wait (frames > dpms_start_frame);
    dpms_start_frame = frames;
    dpms_hsync_seen = 0;
    dpms_vsync_seen = 0;
    while (frames == dpms_start_frame) begin
      @(posedge dvi_clk);
      if (dvi_hsync !== (1'b0 ^ uut.vga_sync_polarity))
        dpms_hsync_seen = 1;
      if (dvi_vsync !== (1'b0 ^ uut.vga_sync_polarity))
        dpms_vsync_seen = 1;
    end

    if (dpms_hsync_seen !== expect_hsync ||
        dpms_vsync_seen !== expect_vsync) begin
      dpms_errors = dpms_errors + 1;
      $display("DPMS MISMATCH level=%0d hsync=%0d/%0d vsync=%0d/%0d",
               level, dpms_hsync_seen, expect_hsync,
               dpms_vsync_seen, expect_vsync);
    end
  end
endtask
`endif

// main
integer i, x, mism, shown, overlay_start_frame;
integer overlay_src_x, overlay_src_y;
reg [31:0] got, exp;
initial begin
  cfg_cmode = 2;
  cfg_scalex = 0;
  cfg_scaley = 0;
  cfg_width = 64;
  if ($value$plusargs("CMODE=%d", cfg_cmode)) ;
  if ($value$plusargs("SCALEX=%d", cfg_scalex)) ;
  if ($value$plusargs("SCALEY=%d", cfg_scaley)) ;
  if ($value$plusargs("WIDTH=%d", cfg_width)) ;

  src_pixels = cfg_width >> cfg_scalex;
  src_lines = NLINES >> cfg_scaley;
  case (cfg_cmode)
    0: words_per_line = src_pixels / 4;
    2: words_per_line = src_pixels;
    default: words_per_line = src_pixels / 2;
  endcase
`ifdef MASTER_DUT
  beats_per_line = words_per_line;
`else
  beats_per_line = (words_per_line + 1) / 2;
`endif
  h_max = cfg_width + 56;

  $display("CONFIG cmode=%0d scalex=%0d scaley=%0d width=%0d words=%0d beats=%0d",
           cfg_cmode, cfg_scalex, cfg_scaley, cfg_width, words_per_line,
           beats_per_line);

  repeat (10) @(negedge aclk);
  aresetn <= 1;
  repeat (4) @(negedge aclk);

  op(OP_DIMENSIONS, (NLINES << 16) | cfg_width);
  op(OP_MAX, (V_MAX << 16) | h_max);
  op(OP_HS, ((cfg_width + 16) << 16) | (cfg_width + 32));
  op(OP_VS, (VS_START << 16) | VS_END);
  op(OP_COLORMODE, cfg_cmode);
  op(OP_SCALE, ((cfg_scaley & 1) << 1) | (cfg_scalex & 1));
  op(OP_SPRITEXY, (2000 << 16) | 2000);
  if (cfg_cmode == 0)
    for (i = 0; i < 256; i = i + 1)
      op(OP_PALETTE, {i[7:0], pal24(i[7:0])});

  stream_en = 1;
  repeat (4) @(negedge aclk);
  dvi_ena = 1;

  wait (frames == 5);

  // The formatter's active-video window opens one dvi_clk before the pixel
  // pipeline delivers word 0 (left edge shows pixel 0 twice on master
  // hardware too), so displayed column x carries source pixel x-1.
  // Without scale_y, capture row r = 0..NLINES-1 shows source line r; with
  // scale_y, capture row r = 1..NLINES shows source line (r-1)>>1.
  mism = 0;
  shown = 0;
  for (i = cfg_scaley; i < NLINES + cfg_scaley; i = i + 1)
    for (x = 1; x < cfg_width; x = x + 1) begin
      got = cap[i * MAXW + x];
      exp = expected_pix(cfg_scaley ? ((i - 1) >> 1) : i, x - 1);
      if (got !== exp) begin
        mism = mism + 1;
        if (shown < 24) begin
          $display("MISMATCH row=%0d x=%0d got=%08x exp=%08x", i, x, got, exp);
          shown = shown + 1;
        end
      end
    end

`ifndef MASTER_DUT
  if (cfg_scalex == 0 && cfg_scaley == 0 && cfg_cmode != 0 &&
      cfg_width >= 32) begin
    op(OP_OVERLAY_POS, (2 << 16) | 8);
    op(OP_OVERLAY_SIZE, (3 << 16) | 9);
    op(OP_OVERLAY_SOURCE_SIZE, (3 << 16) | 9);
    op(OP_OVERLAY_KEY, 0);
    op(OP_OVERLAY_FRAME, 1);
    overlay_stream_en = 1;
    op(OP_OVERLAY_CTRL, 1); /* enable, no key, packed CGX */
    overlay_start_frame = frames;
    wait (frames >= overlay_start_frame + 5);
    if (!early_overlay_prefetch_seen) begin
      mism = mism + 1;
      $display("OVERLAY PREFETCH MISMATCH line 1 was not requested early");
    end

    for (i = 0; i < NLINES; i = i + 1)
      for (x = 1; x < cfg_width; x = x + 1) begin
        got = cap[i * MAXW + x];
        if (i >= 2 && i < 5 && (x - 1) >= 8 && (x - 1) < 17)
          exp = expected_overlay(i - 2);
        else
          exp = expected_pix(i, x - 1);
        exp = exp & 32'h00ffffff;
        if (got !== exp) begin
          mism = mism + 1;
          if (shown < 48) begin
            $display("OVERLAY MISMATCH row=%0d x=%0d got=%08x exp=%08x",
                     i, x, got, exp);
            shown = shown + 1;
          end
        end
      end

    /* Put the same overlay against the right edge. Fetching the next line
     * must not change the current row's independently selected display bank. */
    op(OP_OVERLAY_POS, (2 << 16) | (cfg_width - 9));
    overlay_start_frame = frames;
    wait (frames >= overlay_start_frame + 5);
    for (i = 0; i < NLINES; i = i + 1)
      for (x = 1; x < cfg_width; x = x + 1) begin
        got = cap[i * MAXW + x];
        if (i >= 2 && i < 5 && (x - 1) >= cfg_width - 9)
          exp = expected_overlay(i - 2);
        else
          exp = expected_pix(i, x - 1);
        exp = exp & 32'h00ffffff;
        if (got !== exp) begin
          mism = mism + 1;
          if (shown < 48) begin
            $display("OVERLAY RIGHT MISMATCH row=%0d x=%0d got=%08x exp=%08x",
                     i, x, got, exp);
            shown = shown + 1;
          end
        end
      end

    /* A full-width YUY2 line cannot be delivered during this test mode's
     * short horizontal blank. It remains pixel-exact only if N+1 is fetched
     * concurrently while N is displayed. */
    overlay_stream_width = cfg_width;
    op(OP_OVERLAY_POS, (2 << 16));
    op(OP_OVERLAY_SIZE, (3 << 16) | cfg_width);
    op(OP_OVERLAY_SOURCE_SIZE, (3 << 16) | cfg_width);
    overlay_start_frame = frames;
    wait (frames >= overlay_start_frame + 5);
    for (i = 0; i < NLINES; i = i + 1)
      for (x = 1; x < cfg_width; x = x + 1) begin
        got = cap[i * MAXW + x];
        if (i >= 2 && i < 5)
          exp = expected_overlay(i - 2);
        else
          exp = expected_pix(i, x - 1);
        exp = exp & 32'h00ffffff;
        if (got !== exp) begin
          mism = mism + 1;
          if (shown < 72) begin
            $display("OVERLAY FULL MISMATCH row=%0d x=%0d got=%08x exp=%08x",
                     i, x, got, exp);
            shown = shown + 1;
          end
        end
      end

    /* Resized overlays must remain on the packed-YUY2 hardware plane.
     * Exercise nearest-neighbour upscaling in both axes with distinct luma
     * at every source pixel, so byte/macropixel phase errors are visible. */
    overlay_stream_width = 9;
    overlay_pattern_mode = 1;
    op(OP_OVERLAY_POS, (1 << 16) | 4);
    op(OP_OVERLAY_SIZE, (6 << 16) | 18);
    op(OP_OVERLAY_SOURCE_SIZE, (3 << 16) | 9);
    overlay_start_frame = frames;
    wait (frames >= overlay_start_frame + 6);
    for (i = 0; i < NLINES; i = i + 1)
      for (x = 1; x < cfg_width; x = x + 1) begin
        got = cap[i * MAXW + x];
        if (i >= 1 && i < 7 && (x - 1) >= 4 && (x - 1) < 22) begin
          overlay_src_x = ((x - 1 - 4) * 9) / 18;
          overlay_src_y = ((i - 1) * 3) / 6;
          exp = expected_overlay_pixel(overlay_src_y, overlay_src_x);
        end else begin
          exp = expected_pix(i, x - 1);
        end
        exp = exp & 32'h00ffffff;
        if (got !== exp) begin
          mism = mism + 1;
          if (shown < 96) begin
            $display("OVERLAY UPSCALE MISMATCH row=%0d x=%0d got=%08x exp=%08x",
                     i, x, got, exp);
            shown = shown + 1;
          end
        end
      end

    /* Downscaling must select source coordinates deterministically and may
     * skip source pixels/rows without relabelling the next AXI line. */
    op(OP_OVERLAY_POS, (2 << 16) | 8);
    op(OP_OVERLAY_SIZE, (2 << 16) | 5);
    overlay_start_frame = frames;
    wait (frames >= overlay_start_frame + 6);
    for (i = 0; i < NLINES; i = i + 1)
      for (x = 1; x < cfg_width; x = x + 1) begin
        got = cap[i * MAXW + x];
        if (i >= 2 && i < 4 && (x - 1) >= 8 && (x - 1) < 13) begin
          overlay_src_x = ((x - 1 - 8) * 9) / 5;
          overlay_src_y = ((i - 2) * 3) / 2;
          exp = expected_overlay_pixel(overlay_src_y, overlay_src_x);
        end else begin
          exp = expected_pix(i, x - 1);
        end
        exp = exp & 32'h00ffffff;
        if (got !== exp) begin
          mism = mism + 1;
          if (shown < 120) begin
            $display("OVERLAY DOWNSCALE MISMATCH row=%0d x=%0d got=%08x exp=%08x",
                     i, x, got, exp);
            shown = shown + 1;
          end
        end
      end

    /* Screen-edge clipping must not force the ARM compositor. Negative
     * destination coordinates still map from the original unclipped rect. */
    op(OP_OVERLAY_POS, (16'hffff << 16) | 16'hfffc); /* -4,-1 */
    op(OP_OVERLAY_SIZE, (6 << 16) | 18);
    overlay_start_frame = frames;
    wait (frames >= overlay_start_frame + 6);
    for (i = 0; i < NLINES; i = i + 1)
      for (x = 1; x < cfg_width; x = x + 1) begin
        got = cap[i * MAXW + x];
        if (i < 5 && (x - 1) < 14) begin
          overlay_src_x = ((x - 1 + 4) * 9) / 18;
          overlay_src_y = ((i + 1) * 3) / 6;
          exp = expected_overlay_pixel(overlay_src_y, overlay_src_x);
        end else begin
          exp = expected_pix(i, x - 1);
        end
        exp = exp & 32'h00ffffff;
        if (got !== exp) begin
          mism = mism + 1;
          if (shown < 144) begin
            $display("OVERLAY CLIP MISMATCH row=%0d x=%0d got=%08x exp=%08x",
                     i, x, got, exp);
            shown = shown + 1;
          end
        end
      end

    op(OP_OVERLAY_CTRL, 0);
    overlay_start_frame = frames;
    wait (frames >= overlay_start_frame + 2);
  end

  check_dpms(0, 1, 1); // ON
  check_dpms(1, 0, 1); // STANDBY: HSync off
  check_dpms(2, 1, 0); // SUSPEND: VSync off
  check_dpms(3, 0, 0); // OFF
  op(OP_DPMS, 0);
  mism = mism + dpms_errors;
`endif

  $display("RESULT cmode=%0d scalex=%0d scaley=%0d width=%0d MISMATCHES=%0d",
           cfg_cmode, cfg_scalex, cfg_scaley, cfg_width, mism);
  $finish;
end

// watchdog
initial begin
  #20_000_000;
  $display("RESULT TIMEOUT (frames=%0d)", frames);
  $finish;
end

endmodule
