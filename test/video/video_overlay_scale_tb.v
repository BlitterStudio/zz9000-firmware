`timescale 1ns / 1ps
/*
 * Native PIP overlay scaling across a whole window: every displayed overlay
 * pixel must come from the nearest-neighbour source pixel
 * floor(d * src / dst) in both axes, on every row, including downscales whose
 * vertical step skips source rows irregularly, windows clipped at any screen
 * edge, and an overlay stream too slow to deliver two source rows inside one
 * raster line (which the fetcher must absorb by running ahead).
 *
 * The overlay VDMA model follows the firmware's vblank handoff: frames run
 * back to back (park mode, no fsync); each vblank interrupt stops the
 * channel, commits the next generation FRAME_CYCLES later and restarts row 0
 * RESTART_CYCLES after the interrupt.
 *
 * Plusargs (defaults in brackets):
 *   +CANVAS_W +CANVAS_H [160x96]   screen size
 *   +HBLANK [56] +VBLANK [8]       raster blanking (pixels / lines)
 *   +DVI_HALF_PS [3400]            pixel clock half period (aclk is 100 MHz)
 *   +SRC_W +SRC_H [72x58]          overlay source size
 *   +DST_W +DST_H [72x41]          overlay window size
 *   +POS_X +POS_Y [8,4]            window position (may be negative)
 *   +OVL_GAP_EVERY +OVL_GAP_CYCLES stall the overlay stream GAP_CYCLES
 *                                  after every GAP_EVERY beats
 *   +FRAME_CYCLES [20] +RESTART_CYCLES [60]
 *   +TRACE=1                       log fetch requests/completions
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
module video_overlay_scale_tb;

localparam MAXW = 1280;
localparam MAXH = 720;

localparam OP_COLORMODE = 1;
localparam OP_DIMENSIONS = 2;
localparam OP_SCALE = 4;
localparam OP_MAX = 6;
localparam OP_HS = 7;
localparam OP_VS = 8;
localparam OP_SPRITEXY = 13;
localparam OP_OVERLAY_CTRL = 22;
localparam OP_OVERLAY_POS = 23;
localparam OP_OVERLAY_SIZE = 24;
localparam OP_OVERLAY_KEY = 25;
localparam OP_OVERLAY_SOURCE_SIZE = 26;
localparam OP_OVERLAY_FRAME = 27;

localparam [31:0] FB_WORD = 32'h00123456;

/* Plusarg-driven settings are assigned in the main initial block, never in
 * declarations: declaration initializers may run after it. */
integer canvas_w, canvas_h, src_w, src_h, dst_w, dst_h, pos_x, pos_y;
integer hblank, vblank, dvi_half_ps;
integer ovl_gap_every, ovl_gap_cycles, frame_cycles, restart_cycles;
integer trace_on, trace_frame;
integer h_max, v_max, vs_start, vs_end, beats_per_line;

reg aclk = 0;
reg dvi_clk = 0;
reg dvi_ena = 0;
real dvi_half;
always #5 aclk = ~aclk;                           // 100 MHz
always begin
  if (dvi_half > 0.0) #(dvi_half) dvi_clk = dvi_ena ? ~dvi_clk : 1'b0;
  else #1;
end

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
reg overlay_tlast = 0;
reg overlay_tuser = 0;
reg overlay_tvalid = 0;
wire overlay_tready;

video_formatter uut (
  .m_axis_vid_tdata(tdata),
  .m_axis_vid_tkeep(tkeep),
  .m_axis_vid_tlast(tlast),
  .m_axis_vid_tready(tready),
  .m_axis_vid_tuser(tuser),
  .m_axis_vid_tvalid(tvalid),
  .m_axis_vid_aclk(aclk),
  .aresetn(aresetn),
  .overlay_axis_tdata(overlay_tdata),
  .overlay_axis_tkeep(4'hf),
  .overlay_axis_tlast(overlay_tlast),
  .overlay_axis_tready(overlay_tready),
  .overlay_axis_tuser(overlay_tuser),
  .overlay_axis_tvalid(overlay_tvalid),
  .dvi_clk(dvi_clk),
  .dvi_hsync(dvi_hsync),
  .dvi_vsync(dvi_vsync),
  .dvi_active_video(dvi_active_video),
  .dvi_rgb(dvi_rgb),
  .control_data(control_data),
  .control_op(control_op),
  .control_interlace(1'b0),
  .capture_anchor_toggle(1'b0),
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

/* Luma identifies the source row and column; chroma stays neutral so the
 * converted pixel is a plain gray. */
function [7:0] src_luma(input integer line, input integer pixel);
  src_luma = 8'd16 + ((line * 37 + pixel * 11) % 219);
endfunction

function [31:0] expected_overlay_pixel(input integer line,
                                       input integer pixel);
  integer gray;
  begin
    gray = (298 * (src_luma(line, pixel) - 16) + 128) >> 8;
    expected_overlay_pixel = {8'h00, gray[7:0], gray[7:0], gray[7:0]};
  end
endfunction

// Desktop VDMA: constant 32-bit framebuffer.
reg stream_en = 0;
integer sl, sb;
initial begin : vdma
  wait (stream_en);
  forever begin
    for (sl = 0; sl < canvas_h; sl = sl + 1)
      for (sb = 0; sb < beats_per_line; sb = sb + 1) begin
        @(negedge aclk);
        tdata <= {FB_WORD, FB_WORD};
        tkeep <= 8'hff;
        tuser <= (sl == 0 && sb == 0);
        tlast <= (sb == beats_per_line - 1);
        tvalid <= 1;
        @(posedge aclk);
        while (tready !== 1'b1) @(posedge aclk);
      end
  end
end

// Overlay VDMA, stopped and restarted by the vblank handoff below.
reg overlay_stream_en = 0;
reg overlay_restart = 0;
reg overlay_abort;
integer overlay_generation = 1;
integer ol, ow, og, ovl_beats;
initial begin : overlay_vdma
  ovl_beats = 0;
  wait (overlay_stream_en);
  forever begin
    overlay_abort = 0;
    for (ol = 0; ol < src_h && !overlay_abort; ol = ol + 1)
      for (ow = 0; ow < (src_w + 1) / 2 && !overlay_abort; ow = ow + 1) begin
        @(negedge aclk);
        overlay_tdata <= {8'd128, src_luma(ol, 2 * ow + 1),
                          8'd128, src_luma(ol, 2 * ow)};
        overlay_tuser <= (ol == 0 && ow == 0);
        overlay_tlast <= (ow == (src_w + 1) / 2 - 1);
        overlay_tvalid <= 1;
        while (overlay_tready !== 1'b1 && !overlay_restart)
          @(negedge aclk);
        if (!overlay_restart) begin
          @(posedge aclk);
          ovl_beats = ovl_beats + 1;
          if (ovl_gap_every > 0 && ovl_beats % ovl_gap_every == 0) begin
            @(negedge aclk);
            overlay_tvalid <= 0;
            for (og = 1; og < ovl_gap_cycles; og = og + 1) @(negedge aclk);
          end
        end
        if (overlay_restart)
          overlay_abort = 1;
      end
    @(negedge aclk);
    overlay_tvalid <= 0;
    overlay_tuser <= 0;
    overlay_tlast <= 0;
    /* A finished frame repeats at once (park mode); a reset one restarts
     * after the firmware has reprogrammed the channel. */
    if (overlay_abort) begin
      repeat (restart_cycles) @(negedge aclk);
      overlay_restart = 0;
    end
  end
end

initial begin : overlay_rearm
  wait (overlay_stream_en);
  forever begin
    @(posedge control_vblank[1]);
    overlay_restart = 1;
    repeat (frame_cycles) @(negedge aclk);
    overlay_generation = overlay_generation + 1;
    op(OP_OVERLAY_FRAME, overlay_generation);
  end
end

reg [31:0] cap [0:MAXH*MAXW-1];
reg [11:0] xcnt = 0;
integer row;
always @(posedge dvi_clk) begin
  if (dvi_active_video === 1'b1) begin
    row = uut.counter_y - 1;
    if (row >= 0 && row < canvas_h && xcnt < MAXW)
      cap[row * MAXW + xcnt] <= dvi_rgb;
    xcnt <= xcnt + 1;
  end else
    xcnt <= 0;
end

integer frames = 0;
always @(posedge dvi_clk)
  if (uut.counter_y == vs_start && uut.counter_x == 0)
    frames = frames + 1;

always @(posedge dvi_clk) begin
  if (trace_on && frames == trace_frame) begin
    if (uut.overlay_fetch_request)
      $display("T y=%0d x=%0d REQ line=%0d disp=%0d ready=%0d",
               uut.counter_y, uut.counter_x, uut.overlay_fetch_line,
               uut.overlay_displayed_line, uut.overlay_line_ready);
    if (uut.overlay_linebuffer.completed_line_pixel_valid)
      $display("T y=%0d x=%0d DONE line=%0d", uut.counter_y, uut.counter_x,
               uut.overlay_linebuffer.completed_line_pixel);
  end
end

integer i, x, mism, shown, start_frame, sx, sy, row_bad, bad_rows, first_bad;
reg [31:0] got, exp;
initial begin
  canvas_w = 160; canvas_h = 96; hblank = 56; vblank = 8;
  dvi_half_ps = 3400;
  src_w = 72; src_h = 58; dst_w = 72; dst_h = 41; pos_x = 8; pos_y = 4;
  ovl_gap_every = 0; ovl_gap_cycles = 0;
  frame_cycles = 20; restart_cycles = 60;
  trace_on = 0; trace_frame = -1;
  if ($value$plusargs("CANVAS_W=%d", canvas_w)) ;
  if ($value$plusargs("CANVAS_H=%d", canvas_h)) ;
  if ($value$plusargs("HBLANK=%d", hblank)) ;
  if ($value$plusargs("VBLANK=%d", vblank)) ;
  if ($value$plusargs("DVI_HALF_PS=%d", dvi_half_ps)) ;
  if ($value$plusargs("SRC_W=%d", src_w)) ;
  if ($value$plusargs("SRC_H=%d", src_h)) ;
  if ($value$plusargs("DST_W=%d", dst_w)) ;
  if ($value$plusargs("DST_H=%d", dst_h)) ;
  if ($value$plusargs("POS_X=%d", pos_x)) ;
  if ($value$plusargs("POS_Y=%d", pos_y)) ;
  if ($value$plusargs("OVL_GAP_EVERY=%d", ovl_gap_every)) ;
  if ($value$plusargs("OVL_GAP_CYCLES=%d", ovl_gap_cycles)) ;
  if ($value$plusargs("FRAME_CYCLES=%d", frame_cycles)) ;
  if ($value$plusargs("RESTART_CYCLES=%d", restart_cycles)) ;
  if ($value$plusargs("TRACE=%d", trace_on)) ;
  dvi_half = dvi_half_ps / 1000.0;
  h_max = canvas_w + hblank;
  v_max = canvas_h + vblank;
  vs_start = canvas_h + 2;
  vs_end = canvas_h + 4;
  beats_per_line = (canvas_w + 1) / 2;
  $display("CONFIG canvas=%0dx%0d src=%0dx%0d dst=%0dx%0d pos=%0d,%0d gap=%0d/%0d",
           canvas_w, canvas_h, src_w, src_h, dst_w, dst_h, pos_x, pos_y,
           ovl_gap_every, ovl_gap_cycles);

  repeat (10) @(negedge aclk);
  aresetn <= 1;
  repeat (4) @(negedge aclk);
  op(OP_DIMENSIONS, (canvas_h << 16) | canvas_w);
  op(OP_MAX, (v_max << 16) | h_max);
  op(OP_HS, ((canvas_w + 16) << 16) | (canvas_w + 32));
  op(OP_VS, (vs_start << 16) | vs_end);
  op(OP_COLORMODE, 2);
  op(OP_SCALE, 0);
  op(OP_SPRITEXY, (2000 << 16) | 2000);
  stream_en = 1;
  repeat (4) @(negedge aclk);
  dvi_ena = 1;
  wait (frames == 3);

  op(OP_OVERLAY_POS, ((pos_y & 32'hffff) << 16) | (pos_x & 32'hffff));
  op(OP_OVERLAY_SIZE, (dst_h << 16) | dst_w);
  op(OP_OVERLAY_SOURCE_SIZE, (src_h << 16) | src_w);
  op(OP_OVERLAY_KEY, 0);
  op(OP_OVERLAY_FRAME, 1);
  overlay_stream_en = 1;
  op(OP_OVERLAY_CTRL, 1);
  start_frame = frames;
  trace_frame = start_frame + 4;
  wait (frames >= start_frame + 6);

  mism = 0;
  shown = 0;
  bad_rows = 0;
  first_bad = -1;
  for (i = 0; i < canvas_h; i = i + 1) begin
    row_bad = 0;
    for (x = 0; x < canvas_w; x = x + 1) begin
      got = cap[i * MAXW + x];
      if (i >= pos_y && i < pos_y + dst_h &&
          x >= pos_x && x < pos_x + dst_w) begin
        sx = ((x - pos_x) * src_w) / dst_w;
        sy = ((i - pos_y) * src_h) / dst_h;
        exp = expected_overlay_pixel(sy, sx);
      end else begin
        exp = FB_WORD;
      end
      exp = exp & 32'h00ffffff;
      if (got !== exp) begin
        mism = mism + 1;
        row_bad = 1;
        if (shown < 24) begin
          $display("SCALE MISMATCH row=%0d x=%0d got=%08x exp=%08x",
                   i, x, got, exp);
          shown = shown + 1;
        end
      end
    end
    if (row_bad) begin
      bad_rows = bad_rows + 1;
      if (first_bad < 0)
        first_bad = i;
    end
  end
  $display("RESULT src=%0dx%0d dst=%0dx%0d BAD_ROWS=%0d FIRST_BAD=%0d MISMATCHES=%0d",
           src_w, src_h, dst_w, dst_h, bad_rows, first_bad, mism);
  $finish;
end

endmodule
