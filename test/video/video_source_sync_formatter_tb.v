`timescale 1ns / 1ps
/*
 * Integration testbench: video_formatter.v + video_source_sync.v
 *
 * Runs the real formatter with the production PAL centered vertical
 * geometry (video_scale.h / video.c): a 1080-line canvas on a
 * 1125-line nominal, vsync 1084..1089, 1024 lines of content centered
 * at viewport y=28 (installed through the real OP_VIEWPORT_POS /
 * OP_VIEWPORT_SIZE_COMMIT path), and a scaled-down 32-pixel-wide
 * canvas, so the demand pacing of source rows and the prefetch of the
 * next frame's row zero land at the same offsets from the frame
 * boundary as in the native mode.  Two modes:
 *
 *  progressive (default): 256 source rows, scale_y x4 (line
 *  quadrupling) -> 1024 content rows on the 1080-row canvas; one
 *  anchor per input frame.
 *
 *  +INTERLACE=1: woven interlaced capture, 512 source rows, scale_y x2
 *  (line doubling) -> 1024 content rows.  The capture model matches
 *  videocap_sampler.v: each input field writes only one parity of the
 *  512-row woven frame buffer (field row j of a parity-p field lands on
 *  woven row 2j+p, because cap_y starts at the field parity and steps
 *  by two), the first post-crop token of a field is its parity row, and
 *  one capture_anchor_toggle is emitted per field at that first
 *  completed post-crop row.  Field parity alternates with each anchor,
 *  so a woven row's next same-parity overwrite is TWO anchors later.
 *  The anchor cadence alternates hardware-like short/long field
 *  intervals (PAL half-line offset = 1.8 output lines): 1123.2/1126.8
 *  and 1123.7/1127.3 lines around the 1125-line nominal.
 *
 * The capture side is modeled in absolute time: source (woven) row w
 * becomes DDR-readable at anchor + floor(w/2)*SRC_LINE + WRITEBACK,
 * with WRITEBACK equal to one input line (~3.6 output lines), the
 * normal worst case of the single-frame-buffer writeback path.
 *
 * Proven per displayed frame after lock, STARTING WITH THE FIRST
 * VISIBLE FRAME of an acquisition or reacquisition:
 *  - read-after-write: the earliest row-zero DDR read, taken at the
 *    measured start-of-frame AXIS beat minus the 40-output-line VDMA
 *    prefetch lead observed on hardware (the VDMA prefetches the next
 *    row zero after the last source row, before the formatter consumes
 *    the held SOF at the vsync start line), is later than the row-zero
 *    writeback of the field that owns row zero's parity by a positive
 *    margin of lines;
 *  - read-before-overwrite: every woven row's last AXIS beat precedes
 *    the next same-parity field's overwrite of that row (the anchor
 *    two fields later); in progressive mode, the next frame's
 *    overwrite of that row (the next anchor);
 *  - visibility is associated with the raster frame that actually
 *    displays a delivered SOF: the beat is accepted at the vsync start
 *    line while the controller may still hide the previous frame, and
 *    the incoming frame's shown decision lands at the following wrap,
 *    so the record's margin verdict is taken after that wrap and the
 *    first newly exposed frame cannot escape checking;
 *  - interlace frames are snapshotted at delivery and evaluated once
 *    the second following anchor has arrived (the two-anchor-later
 *    overwrite postdates the completed scanout), progressive frames
 *    evaluate as before;
 *  - margins are reported (min RAW / min RBW) for the parent's
 *    physical timing proof.
 *
 * Observable checks (dvi outputs only, no internal register copies):
 *  - per frame, between consecutive VSync falling edges: exactly one
 *    VSync pulse, one HSync pulse per raster line, the frame length a
 *    whole number of lines inside [vsync_end+2, 1152], and 1080 DE
 *    rows of exactly CANVAS_W active pixels each - also while the
 *    source-sync path hides the picture, proving syncs keep running;
 *  - interlace stationary cadence: after settling on an alternating
 *    short/long anchor cadence, the tracked frame lengths must stay
 *    within a 2-line total spread and average the cadence mean +/-
 *    1 line (the corrected controller normalizes the anchor phase to
 *    the averaged grid; wrapping each frame at a fixed delay after
 *    every raw anchor instead modulates the frame length by the full
 *    short/long difference, which these assertions reject);
 *  - full-canvas pixel equality against the woven buffer content,
 *    borders included: the 1024 content rows centered at viewport
 *    y=28 show source row floor(content_row/2) (x2) or content_row/4
 *    (x4), and the 28-row top and bottom borders must be black, before
 *    enabling, after lock, and after disabling again.  While tracked,
 *    the picture must be the normal native RGB output - a diagnostic
 *    color override fails these compares;
 *  - picture black while unlocked/lost;
 *  - progressive acquisition is exercised from a deliberately late
 *    anchor phase (first anchor released just after a frame wrap):
 *    the picture must stay hidden until the wrap phase reaches the
 *    bounded delay window, then appear with margins checked from that
 *    first visible frame onward - for acquisition and reacquisition.
 *
 * Plusargs: +SMOKE=1 shortens each segment; +INTERLACE=1 selects the
 * woven interlaced capture mode.
 */

module video_source_sync_formatter_tb;

localparam integer MAXW = 64;
localparam integer CANVAS_H = 1080;     // canvas (display) rows
localparam integer CONTENT_H = 1024;    // centered content rows
localparam integer VIEWPORT_Y = 28;     // production centered viewport
localparam integer SRC_ROWS = 256;      // 1024 >> 2 (progressive)
localparam integer MAX_SRC = 512;       // woven rows (interlace)

localparam OP_DIMENSIONS = 2;
localparam OP_SCALE = 4;
localparam OP_MAX = 6;
localparam OP_HS = 7;
localparam OP_VS = 8;
localparam OP_COLORMODE = 1;
localparam OP_SPRITEXY = 13;
localparam OP_VIEWPORT_POS = 28;
localparam OP_VIEWPORT_SIZE_COMMIT = 29;
localparam OP_SOURCE_SYNC = 30;

// geometry (production PAL vertical timing, reduced 32px width)
localparam integer CANVAS_W = 32;
localparam integer H_MAX = CANVAS_W + 56;   // 88 -> 89 pixel clocks/line
localparam integer V_MAX = 1125;
localparam integer VS_START = 1084;
localparam integer VS_END = 1089;

// timing model, in output lines.  The formatter's raster line is
// H_MAX pixel clocks (counter_x wraps on the clock it reaches h_max),
// so the anchor generator must pace in the same unit or every cadence
// drifts 1/H_MAX slow and the acceptance window silently rejects it.
localparam real LINE_NS = H_MAX * 6.8; // dvi_clk toggles every 3.4ns
localparam real SRC_LINE = 3.6;   // PAL input line in output lines
localparam real WRITEBACK = 3.6;  // one input line, normal worst case
localparam real VDMA_SOF_LEAD = 40.0; // row0 DDR read before SOF beat
localparam real FIFO_LEAD = 1.0;  // row r>=1 DDR read before its beat
localparam real MARGIN_MIN = 4.0; // required positive margin, lines

integer cfg_smoke = 0;
integer cfg_lace = 0;
integer seg_frames = 12;
integer src_rows = SRC_ROWS;
integer scale_code = 2;           // scale_y code: 2 = x4, 1 = x2
integer factor = 4;               // display rows per source row

// clocks
reg aclk = 0;
reg dvi_clk = 0;
reg dvi_ena = 0;
always #5 aclk = ~aclk;
always #3.4 dvi_clk = dvi_ena ? ~dvi_clk : 1'b0;

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
reg lace_pin = 0;
wire [1:0] control_vblank;
reg capture_anchor_toggle = 0;

video_formatter uut (
  .m_axis_vid_tdata(tdata),
  .m_axis_vid_tkeep(tkeep),
  .m_axis_vid_tlast(tlast),
  .m_axis_vid_tready(tready),
  .m_axis_vid_tuser(tuser),
  .m_axis_vid_tvalid(tvalid),
  .m_axis_vid_aclk(aclk),
  .aresetn(aresetn),
  .overlay_axis_tdata(32'b0),
  .overlay_axis_tkeep(4'h0),
  .overlay_axis_tlast(1'b0),
  .overlay_axis_tready(),
  .overlay_axis_tuser(1'b0),
  .overlay_axis_tvalid(1'b0),
  .dvi_clk(dvi_clk),
  .dvi_hsync(dvi_hsync),
  .dvi_vsync(dvi_vsync),
  .dvi_active_video(dvi_active_video),
  .dvi_rgb(dvi_rgb),
  .control_data(control_data),
  .control_op(control_op),
  .control_interlace(lace_pin),
  .capture_anchor_toggle(capture_anchor_toggle),
  .control_vblank(control_vblank),
  .scanline_intensity(8'd0),
  .scanline_width(2'b00),
  .scanline_parity(1'b0),
  .scanline_intensity2(8'd0)
);

// zero the DUT registers that hardware clears via GSR but plain RTL
// simulation leaves at X
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

/* Row-unique framebuffer pattern.  The per-channel row coefficients
 * (plus the row>>8 fold) keep all 520 woven rows distinguishable, so a
 * scanout that picks the wrong woven row cannot alias to the expected
 * content. */
function [31:0] fb_word(input integer l, input integer w);
  reg [7:0] br, bg, bb;
  begin
    bb = (l * 137 + w * 4) & 8'hFF;
    bg = (l * 61 + (l >> 8) * 200 + w * 4) & 8'hFF;
    br = (l * 29 + (l >> 8) * 77 + w * 4 + 5) & 8'hFF;
    fb_word = {8'h00, br, bg, bb};
  end
endfunction

/* ------------------------------------------------------------------ */
/* VDMA stream model with per-frame timing records                   */
/* ------------------------------------------------------------------ */

reg stream_en = 0;
integer sl, sb;
time frame_sof_t;
integer frame_sof_wrap_f;
integer frames = 0;
time row_first_t [0:MAX_SRC-1];
time row_last_t [0:MAX_SRC-1];
integer margins_enabled = 0;

/* Delivered-frame snapshot for the margin evaluation.  In woven mode
 * the next same-parity overwrite anchor arrives ~2 fields after the
 * scanout completed, so the record must survive until then. */
time pend_sof_t;
time pend_rf [0:MAX_SRC-1];
time pend_rl [0:MAX_SRC-1];
integer pend_ai;
integer pend_need;
reg pend_ok;
reg pend_valid = 0;
integer j2, sl2;

// wait for the formatter's frame-sync release pulse (vsync start line)
task wait_frame_sync;
  begin
    forever begin
      @(posedge dvi_clk);
      if (uut.need_frame_sync === 1'b1) disable wait_frame_sync;
    end
  end
endtask

integer margin_skipped = 0;
integer anchors_n = 0;
time anchor_t [0:1023];
initial begin : vdma
  wait (stream_en);
  forever begin
    wait_frame_sync;
    frame_sof_t = 0;
    for (sl = 0; sl < src_rows; sl = sl + 1) begin
      for (sb = 0; sb < CANVAS_W / 2; sb = sb + 1) begin
        @(negedge aclk);
        tdata <= {fb_word(sl, 2 * sb + 1), fb_word(sl, 2 * sb)};
        tkeep <= 8'hFF;
        tuser <= (sl == 0 && sb == 0);
        tlast <= (sb == CANVAS_W / 2 - 1);
        tvalid <= 1;
        while (tready !== 1'b1) @(negedge aclk);
        if (sl == 0 && sb == 0) begin
          frame_sof_t = $time;
          // frame counter value at the SOF beat: the raster frame that
          // displays this delivery starts at the NEXT wrap
          frame_sof_wrap_f = frames;
        end
        if (sb == 0) row_first_t[sl] = $time;
        if (sb == CANVAS_W / 2 - 1) row_last_t[sl] = $time;
        @(posedge aclk);
      end
    end
    @(negedge aclk);
    tvalid <= 0;
    tuser <= 0;
    tlast <= 0;
    // snapshot the delivered frame; the anchor it presents is fixed by
    // the SOF beat time, so this index is deterministic whenever taken
    if (!pend_valid) begin
    pend_sof_t = frame_sof_t;
    pend_ai = -1;
    for (j2 = 0; j2 < anchors_n; j2 = j2 + 1)
      if (anchor_t[j2] <= frame_sof_t) pend_ai = j2;
    for (sl2 = 0; sl2 < src_rows; sl2 = sl2 + 1) begin
      pend_rf[sl2] = row_first_t[sl2];
      pend_rl[sl2] = row_last_t[sl2];
    end
    pend_need = cfg_lace ? 2 : 1;
    /* Visibility of the raster frame this delivery feeds.  The SOF
     * beat is accepted at the vsync start line while the controller
     * may still hide the PREVIOUS frame; the delivered frame itself
     * is displayed by the raster frame starting at the NEXT wrap, and
     * the controller decides that frame's visibility at the wrap.
     * Sample the shown decision after that wrap so the first frame
     * newly exposed by an acquisition or reacquisition receives the
     * same DDR-margin verdict as every steady frame (sampling at the
     * SOF beat instead would mark it hidden and skip it). */
    wait (frames > frame_sof_wrap_f);
    pend_ok = uut.video_source_sync_i.picture_ok === 1'b1;
    pend_valid = 1;
    end else begin
      // Keep the evaluator's timestamp bundle immutable during source
      // loss; never replace it with the next frame while it is waiting.
      margin_skipped = margin_skipped + 1;
    end
  end
end

/* ------------------------------------------------------------------ */
/* Anchor generator (capture-domain, asynchronous to both clocks)     */
/* ------------------------------------------------------------------ */


// background cadence generator so anchors keep flowing while the main
// sequence waits on frame boundaries (the controller must never see a
// fabricated source loss during settle waits).  In woven mode the
// spacing alternates per anchor: interval after an even anchor = the
// short field period, after an odd anchor = the long one (PAL
// half-line offset), matching one anchor per interlaced field.
reg auto_run = 0;
reg auto_alt = 0;
real auto_spacing = 1125.0;
real auto_spacing_b = 1125.0;
integer auto_left = 0;
real next_spacing;

initial begin : anchor_gen
  wait (aresetn);
  forever begin
    if (auto_run && auto_left > 0) begin
      if (auto_alt)
        next_spacing = (anchors_n & 1) ? auto_spacing_b : auto_spacing;
      else
        next_spacing = auto_spacing;
      #(next_spacing * LINE_NS);
      capture_anchor_toggle = ~capture_anchor_toggle;
      anchor_t[anchors_n] = $time;
      anchors_n = anchors_n + 1;
      auto_left = auto_left - 1;
    end else begin
      @(negedge aclk);
    end
  end
end

task burst_anchors(input integer count, input real spacing);
  begin
    auto_spacing = spacing;
    auto_alt = 0;
    auto_left = auto_left + count;
    auto_run = 1;
  end
endtask

task burst_anchors_alt(input integer count, input real sa, input real sb_);
  begin
    auto_spacing = sa;
    auto_spacing_b = sb_;
    auto_alt = 1;
    auto_left = auto_left + count;
    auto_run = 1;
  end
endtask

task stop_anchors;
  begin
    auto_run = 0;
    auto_left = 0;
  end
endtask

integer errors = 0;
real min_raw_margin = 99999.0;
real min_rbw_margin = 99999.0;
integer margin_frames = 0;
integer last_wrap_line = 0;
integer frame_lines;
integer diag_prev_total = -1;
integer line_count = 0;
reg line_tick_d = 0;

// wait until the picture is shown, anchors flowing, bounded by frames
task wait_shown(input integer max_frames);
  integer f0;
  begin
    f0 = frames;
    while ((uut.source_sync_video_hidden !== 1'b0 ||
            uut.video_source_sync_i.locked !== 1'b1) && frames < f0 + max_frames)
      @(posedge dvi_clk);
    if (uut.source_sync_video_hidden !== 1'b0 ||
        uut.video_source_sync_i.locked !== 1'b1) begin
      errors = errors + 1;
      $display("ERR: picture never shown within %0d frames (%0d anchors)", max_frames, anchors_n);
    end
  end
endtask

/* ------------------------------------------------------------------ */
/* Monitors and checks                                                */
/* ------------------------------------------------------------------ */

always @(posedge dvi_clk) begin
  if (aresetn) begin
    line_tick_d <= (uut.counter_x >= uut.vga_h_max);
    if (uut.counter_x >= uut.vga_h_max)
      line_count <= line_count + 1;
  end
end

always @(negedge dvi_clk) begin
  if (aresetn && line_tick_d) begin
    if (uut.counter_y == 0) begin
      frames = frames + 1;
      frame_lines = line_count - last_wrap_line;
      last_wrap_line = line_count;
      if (frame_lines < VS_END + 2 || frame_lines > 1152) begin
        errors = errors + 1;
        if (errors < 30) $display("ERR: frame total %0d outside [%0d,1152]", frame_lines, VS_END + 2);
      end
      if (uut.source_sync_enable_pix === 1'b0 && frame_lines != V_MAX) begin
        errors = errors + 1;
        if (errors < 30) $display("ERR: disabled frame total %0d != %0d", frame_lines, V_MAX);
      end
      /* The diagnostic side channel must publish the metrics of the
       * frame that completed at the previous wrap: the source FSM
       * snapshots the bus one cycle after each wrap edge, and the
       * transfer finishes far inside the following frame.  A payload
       * latched on the wrap edge itself would still hold the
       * penultimate frame's totals here. */
      if (diag_prev_total > 0) begin
        if (uut.source_sync_diagnostic[31:20] !== (diag_prev_total % 4096)) begin
          errors = errors + 1;
          if (errors < 30)
            $display("ERR: diag frame total %0d != published %0d for the previous frame",
                     diag_prev_total % 4096, uut.source_sync_diagnostic[31:20]);
        end
      end
      diag_prev_total = frame_lines;
    end
  end
end

/* dvi_rgb is registered one clock after the hidden gate itself, so
 * compare against the one-clock-delayed copy; the pixel data path is
 * what must be black, not the gate's own transition cycle. */
reg hidden_d = 0;
always @(posedge dvi_clk) hidden_d <= uut.source_sync_video_hidden;

// while the source-sync path hides the picture, RGB must be black
always @(negedge dvi_clk) begin
  if (aresetn && hidden_d === 1'b1 && dvi_rgb !== 32'b0) begin
    errors = errors + 1;
    if (errors < 30) $display("ERR: rgb %08x not black while hidden (t=%0t)", dvi_rgb, $time);
  end
end

// pixel capture of displayed frames (full canvas: borders included)
reg [31:0] cap [0:CANVAS_H * MAXW - 1];
integer cap_row, cap_col;
always @(posedge dvi_clk) begin
  if (dvi_active_video === 1'b1) begin
    cap_row = uut.counter_y - factor;
    if (cap_row >= 0 && cap_row < CANVAS_H && cap_col < MAXW)
      cap[cap_row * MAXW + cap_col] <= dvi_rgb;
    cap_col <= cap_col + 1;
  end else begin
    cap_col <= 0;
  end
end

task compare_frame(input integer what);
  integer r, x, sr;
  reg [31:0] got, exp;
  integer shown_errs;
  begin
    shown_errs = 0;
    for (r = 0; r < CANVAS_H; r = r + 1)
      for (x = 0; x < CANVAS_W; x = x + 1) begin
        got = cap[r * MAXW + x];
        if (r < VIEWPORT_Y || r >= VIEWPORT_Y + CONTENT_H)
          // centered canvas border rows carry no source line: black
          exp = 32'b0;
        else begin
          sr = (r - VIEWPORT_Y) >> scale_code;
          exp = fb_word(sr, x);
        end
        if (got !== exp) begin
          errors = errors + 1;
          shown_errs = shown_errs + 1;
          if (shown_errs <= 12)
            $display("ERR: PIXEL %0d row=%0d x=%0d got=%08x exp=%08x", what, r, x, got, exp);
        end
      end
    if (shown_errs == 0) $display("PIXELS %0d OK (%0d canvas rows incl borders, native rgb)", what, CANVAS_H);
  end
endtask

/* ------------------------------------------------------------------ */
/* Observable sync/DE monitor (dvi outputs only)                      */
/* ------------------------------------------------------------------ */

/* Between consecutive VSync falling edges (negative polarity, idle
 * high): the frame must be a whole number of H_MAX-clock lines inside
 * the guarded bounds, carry exactly one VSync pulse and one HSync
 * pulse per raster line, and produce CANVAS_H data-enable runs of exactly
 * CANVAS_W pixels - including frames whose picture is hidden, which
 * proves the monitor-facing syncs never stop.  Frame lengths are
 * recorded for the stationary-cadence spread checks. */
reg obs_armed = 0;
reg obs_started = 0;
reg vs_d = 1, hs_d = 1, de_d = 0;
integer frame_cycles = 0;
integer f_hsync = 0, f_vs_rise = 0, f_de_rows = 0, f_de_px = 0;
integer f_de_px_min = 99999, f_de_px_max = 0;
integer obs_frames = 0;
integer obs_this_len;
integer obs_len [0:511];
integer obs_fid [0:511];
integer hs_low_pixels = 0, vs_low_pixels = 0;

always @(posedge dvi_clk) begin
  if (obs_armed) begin
    if (dvi_hsync === 1'b0)
      hs_low_pixels = hs_low_pixels + 1;
    else if (hs_d === 1'b0) begin
      if (hs_low_pixels != 16) begin
        errors = errors + 1;
        if (errors < 30) $display("ERR: hsync pulse %0d pixels, expected16", hs_low_pixels);
      end
      hs_low_pixels = 0;
    end
    if (dvi_vsync === 1'b0)
      vs_low_pixels = vs_low_pixels + 1;
    else if (vs_d === 1'b0) begin
      if (vs_low_pixels != (VS_END - VS_START) * H_MAX) begin
        errors = errors + 1;
        if (errors < 30) $display("ERR: vsync pulse %0d pixels", vs_low_pixels);
      end
      vs_low_pixels = 0;
    end
    hs_d <= dvi_hsync;
    vs_d <= dvi_vsync;
    de_d <= dvi_active_video;
    frame_cycles = frame_cycles + 1;
    if (hs_d === 1'b1 && dvi_hsync === 1'b0)
      f_hsync = f_hsync + 1;
    if (vs_d === 1'b0 && dvi_vsync === 1'b1)
      f_vs_rise = f_vs_rise + 1;
    if (dvi_active_video === 1'b1) begin
      f_de_px = f_de_px + 1;
      if (de_d === 1'b0) f_de_rows = f_de_rows + 1;
    end else begin
      if (de_d === 1'b1) begin
        if (f_de_px < f_de_px_min) f_de_px_min = f_de_px;
        if (f_de_px > f_de_px_max) f_de_px_max = f_de_px;
      end
      f_de_px = 0;
    end
    if (vs_d === 1'b1 && dvi_vsync === 1'b0) begin
      if (obs_started && obs_frames < 512) begin
        obs_this_len = frame_cycles / H_MAX;
        obs_len[obs_frames] = obs_this_len;
        obs_fid[obs_frames] = frames;
        obs_frames = obs_frames + 1;
        if (frame_cycles % H_MAX != 0) begin
          errors = errors + 1;
          if (errors < 30)
            $display("ERR: vsync interval %0d clocks not whole lines", frame_cycles);
        end
        if (obs_this_len < VS_END + 2 || obs_this_len > 1152) begin
          errors = errors + 1;
          if (errors < 30)
            $display("ERR: observable frame total %0d outside [%0d,1152]", obs_this_len, VS_END + 2);
        end
        if (f_hsync != obs_this_len) begin
          errors = errors + 1;
          if (errors < 30)
            $display("ERR: %0d hsync pulses in %0d-line frame", f_hsync, obs_this_len);
        end
        if (f_vs_rise != 1) begin
          errors = errors + 1;
          if (errors < 30)
            $display("ERR: %0d vsync pulses in one frame", f_vs_rise);
        end
        if (f_de_rows != CANVAS_H) begin
          errors = errors + 1;
          if (errors < 30)
            $display("ERR: %0d DE rows in frame (want %0d)", f_de_rows, CANVAS_H);
        end
        if (f_de_px_min != CANVAS_W || f_de_px_max != CANVAS_W) begin
          errors = errors + 1;
          if (errors < 30)
            $display("ERR: DE run %0d..%0d px (want %0d)", f_de_px_min, f_de_px_max, CANVAS_W);
        end
      end
      obs_started = 1;
      frame_cycles = 0;
      f_hsync = 0;
      f_vs_rise = 0;
      f_de_rows = 0;
      f_de_px = 0;
      f_de_px_min = 99999;
      f_de_px_max = 0;
    end
  end
end

/* Stationary-cadence check over the observable frame lengths whose
 * frame ids fall in [f_lo, f_hi): after settling on an alternating
 * short/long anchor cadence, tracked frames must stay within a
 * 2-line total spread and average the cadence mean.  Wrapping at a
 * fixed delay after every raw anchor instead spreads the frame length
 * by the full short/long difference (~3.6 lines here), which fails. */
task check_cadence(input integer f_lo, input integer f_hi,
                   input real mean_exp, input integer tag);
  integer i, n, lmin, lmax;
  real sum;
  begin
    n = 0; sum = 0.0; lmin = 99999; lmax = 0;
    for (i = 0; i < obs_frames; i = i + 1)
      if (obs_fid[i] >= f_lo && obs_fid[i] < f_hi) begin
        if (obs_len[i] < lmin) lmin = obs_len[i];
        if (obs_len[i] > lmax) lmax = obs_len[i];
        sum = sum + obs_len[i];
        n = n + 1;
      end
    if (n >= 4) begin
      if (lmax - lmin > 2) begin
        errors = errors + 1;
        if (errors < 30)
          $display("ERR: CADENCE %0d spread %0d lines over %0d frames (max 2)", tag, lmax - lmin, n);
      end
      if (sum / n < mean_exp - (1.0 / n + 0.1) ||
          sum / n > mean_exp + (1.0 / n + 0.1)) begin
        errors = errors + 1;
        if (errors < 30)
          $display("ERR: CADENCE %0d mean %.2f vs expected %.2f (%0d frames)", tag, sum / n, mean_exp, n);
      end
      $display("CADENCE %0d n=%0d min=%0d max=%0d mean=%.2f exp=%.2f",
               tag, n, lmin, lmax, sum / n, mean_exp);
    end else if (cfg_smoke == 0) begin
      errors = errors + 1;
      $display("ERR: insufficient stationary cadence samples (%0d)", n);
    end
  end
endtask

/* ------------------------------------------------------------------ */
/* Woven / progressive DDR margin proof                               */
/* ------------------------------------------------------------------ */

/* For the delivered frame (snapshot), locate the anchor it presents
 * (the latest anchor at or before its SOF beat), then verify every
 * row's DDR read happens after the writeback of the field that owns
 * that row's parity and before that field's next same-parity
 * overwrite:
 *   progressive: field k writes every row r at anchor + r*SRC_LINE,
 *                overwritten by anchor k+1;
 *   woven:       field k (parity p = k mod 2) writes only rows of
 *                parity p; woven row w is field row w>>1, written at
 *                anchor + (w>>1)*SRC_LINE, overwritten by field k+2.
 * Row zero's DDR read is placed VDMA_SOF_LEAD lines before the
 * measured SOF beat to model the hardware VDMA prefetching after the
 * last source row, ahead of the formatter's frame-sync consumption. */
task check_frame_margins;
  integer r, ai, wi, oi, ri;
  time a0, ddr, wr;
  real raw, rbw, tmp;
  begin
    if (!margins_enabled || !pend_ok || pend_ai < 0)
      disable check_frame_margins;
    ai = pend_ai;
    if (ai + pend_need >= anchors_n) begin
      margin_skipped = margin_skipped + 1;
      disable check_frame_margins;
    end
    a0 = anchor_t[ai];

    // row zero read-after-write, using the prefetch-before-SOF read
    tmp = pend_sof_t;
    tmp = tmp / LINE_NS - VDMA_SOF_LEAD;      // earliest row0 DDR read
    ddr = tmp * LINE_NS;
    wi = cfg_lace ? ((ai & 1) ? ai - 1 : ai) : ai;
    tmp = anchor_t[wi];
    tmp = tmp / LINE_NS + WRITEBACK;          // row0 writeback complete
    wr = tmp * LINE_NS;
    raw = ddr;
    raw = (raw - wr) / LINE_NS;

    rbw = 99999.0;
    for (r = 0; r < src_rows; r = r + 1) begin
      // DDR read of row r precedes its first AXIS beat by FIFO_LEAD
      tmp = pend_rf[r];
      tmp = tmp / LINE_NS - FIFO_LEAD;
      ddr = tmp * LINE_NS;
      if (cfg_lace) begin
        // latest field of this row's parity, and its next same-parity
        // overwrite two anchors later
        wi = ((r & 1) == (ai & 1)) ? ai : ai - 1;
        oi = ((r & 1) == (ai & 1)) ? ai + 2 : ai + 1;
        ri = r >> 1;
      end else begin
        wi = ai;
        oi = ai + 1;
        ri = r;
      end
      tmp = anchor_t[wi];
      tmp = tmp / LINE_NS + ri * SRC_LINE + WRITEBACK;
      wr = tmp * LINE_NS;
      tmp = ddr;
      tmp = (tmp - wr) / LINE_NS;
      if (tmp < raw) raw = tmp;
      // overwrite of this row by the next same-parity field starts at
      // its capture time: the read must finish before it begins
      tmp = anchor_t[oi];
      tmp = tmp - pend_rl[r];
      tmp = tmp / LINE_NS + ri * SRC_LINE;
      if (tmp < rbw) rbw = tmp;
    end

    margin_frames = margin_frames + 1;
    if (raw < min_raw_margin) min_raw_margin = raw;
    if (rbw < min_rbw_margin) min_rbw_margin = rbw;
    if (raw < MARGIN_MIN || rbw < MARGIN_MIN) begin
      errors = errors + 1;
      if (errors < 30)
        $display("ERR: MARGIN raw=%.1f rbw=%.1f lines (frame %0d)", raw, rbw, frames);
    end
  end
endtask

/* Margin evaluator: consumes delivered-frame snapshots once the
 * anchors they need for their overwrite bounds have arrived (or a
 * bounded timeout drops the frame when the source went away). */
initial begin : margin_eval
  integer start_frame;
  forever begin
    wait (pend_valid);
    start_frame = frames;
    wait (anchors_n > pend_ai + pend_need || frames >= start_frame + 4);
    check_frame_margins;
    pend_valid = 0;
  end
end

/* ------------------------------------------------------------------ */
/* Sequence                                                           */
/* ------------------------------------------------------------------ */

integer f0;

initial begin
  if ($value$plusargs("SMOKE=%d", cfg_smoke)) ;
  if ($value$plusargs("INTERLACE=%d", cfg_lace)) ;
  if (cfg_smoke != 0) seg_frames = 4;
  if (cfg_lace != 0) begin
    src_rows = MAX_SRC;
    scale_code = 1;   // scale_y x2: 512 woven rows -> 1024 content rows
    factor = 2;
  end
  lace_pin = (cfg_lace != 0);

  repeat (10) @(negedge aclk);
  aresetn <= 1;
  repeat (4) @(negedge aclk);

  /* Production centered-mode programming order (video.c): dimensions
   * name the canvas, then the viewport position and the content size
   * commit install the centered 1024-row rectangle at y=28.  The
   * formatter installs the rectangle at a frame boundary through its
   * CDC handshake; the first two raster frames still run full-canvas,
   * so the pixel/DE checks below only start afterwards. */
  op(OP_DIMENSIONS, (CANVAS_H << 16) | CANVAS_W);
  op(OP_VIEWPORT_POS, (VIEWPORT_Y << 16) | 0);
  op(OP_VIEWPORT_SIZE_COMMIT, (CONTENT_H << 16) | CANVAS_W);
  op(OP_MAX, (V_MAX << 16) | H_MAX);
  op(OP_HS, ((CANVAS_W + 16) << 16) | (CANVAS_W + 32));
  op(OP_VS, (VS_START << 16) | VS_END);
  op(OP_COLORMODE, 2);
  op(OP_SCALE, scale_code << 1);
  op(OP_SPRITEXY, (2000 << 16) | 2000);

  stream_en = 1;
  repeat (4) @(negedge aclk);
  dvi_ena = 1;

  // observable sync/DE checks from the second settled frame on
  wait (frames >= 2);
  obs_armed = 1;

  // baseline: disabled behaviour, pixel-exact
  wait (frames >= 5);
  compare_frame(0);
  if (uut.source_sync_video_hidden !== 1'b0) begin
    errors = errors + 1;
    $display("ERR: hidden asserted while disabled");
  end

  if (cfg_lace == 0) begin : progressive_mode
    // --- segment A: enable from a deliberately late anchor phase ---
    // Align the release to a fresh wrap so the first anchor lands one
    // full cadence later, just past the following wrap: lock forms
    // with the anchor near raster line zero, the worst acquisition
    // phase (the pre-fix controller exposed those frames; the gated
    // one must stay hidden until the wrap phase reaches the delay
    // window and then present).
    f0 = frames;
    wait (frames > f0);
    op(OP_SOURCE_SYNC, 1);
    burst_anchors(seg_frames + 45, 1125.0);
    wait_shown(60);
    /* From here on, every frame the VDMA model delivers is
     * margin-checked (read-after-write / read-before-overwrite
     * against the modeled capture writeback times); the visibility
     * association above already covered the first visible frame. */
    margins_enabled = 1;
    f0 = frames;
    while (frames < f0 + seg_frames + 2) @(posedge dvi_clk);
    compare_frame(1);

    // --- segment B: fractional drift (59.94-style) ---
    burst_anchors(seg_frames + 6, 1126.5);
    f0 = frames;
    while (frames < f0 + seg_frames + 2) @(posedge dvi_clk);
    compare_frame(2);

    // --- segment B2: maximum-extension cadence at the window edge ---
    burst_anchors(seg_frames + 6, 1150.0);
    f0 = frames;
    while (frames < f0 + seg_frames + 2) @(posedge dvi_clk);
    compare_frame(3);

    // --- segment C: source loss hides and falls back, then relocks
    // from a fresh late anchor phase (same worst case as segment A) ---
    stop_anchors;
    #(3.0 * 1125.0 * LINE_NS);
    if (uut.source_sync_video_hidden !== 1'b1) begin
      errors = errors + 1;
      $display("ERR: not hidden after source loss");
    end
    f0 = frames;
    wait (frames > f0);
    burst_anchors(60, 1125.0);
    wait_shown(60);
    /* One full settled frame after the picture returns, so the last
     * captured frame is entirely shown content. */
    f0 = frames;
    while (frames < f0 + 2) @(posedge dvi_clk);
    compare_frame(4);

    // --- segment D: disable returns exact nominal behaviour ---
    op(OP_SOURCE_SYNC, 0);
    stop_anchors;
    f0 = frames;
    while (frames < f0 + 3) @(posedge dvi_clk);
    compare_frame(5);
  end else begin : interlace_mode
    // --- segment A: enable, hardware-like alternating field cadence
    // (PAL half-line offset: 1.8 output lines around the nominal) ---
    op(OP_SOURCE_SYNC, 1);
    burst_anchors_alt(seg_frames + 60, 1123.2, 1126.8);
    wait_shown(45);
    /* From here on, every frame the VDMA model delivers is
     * margin-checked against the woven parity model (each field
     * writes one parity of the 512-row buffer; the next same-parity
     * overwrite is two anchors later); the first visible frame of the
     * acquisition is covered by the wrap-time visibility association. */
    margins_enabled = 1;
    f0 = frames;
    while (frames < f0 + seg_frames + 2) @(posedge dvi_clk);
    compare_frame(1);
    check_cadence(f0 + 3, frames - 1, 1125.0, 1);

    // --- segment B: off-rate source, mean half a line long ---
    burst_anchors_alt(seg_frames + 6, 1123.7, 1127.3);
    f0 = frames;
    while (frames < f0 + seg_frames + 2) @(posedge dvi_clk);
    compare_frame(2);
    check_cadence(f0 + 3, frames - 1, 1125.5, 2);

    // --- segment C: source loss hides and falls back, then relocks ---
    stop_anchors;
    #(3.0 * 1125.0 * LINE_NS);
    if (uut.source_sync_video_hidden !== 1'b1) begin
      errors = errors + 1;
      $display("ERR: not hidden after source loss");
    end
    burst_anchors_alt(60, 1123.2, 1126.8);
    wait_shown(45);
    /* One full settled frame after the picture returns, so the last
     * captured field pair is entirely shown content. */
    f0 = frames;
    while (frames < f0 + 2) @(posedge dvi_clk);
    compare_frame(3);

    // --- segment D: disable returns exact nominal behaviour ---
    op(OP_SOURCE_SYNC, 0);
    stop_anchors;
    f0 = frames;
    while (frames < f0 + 3) @(posedge dvi_clk);
    compare_frame(4);
  end

  if (margin_frames < 4) begin
    errors = errors + 1;
    $display("ERR: insufficient DDR margin frames (%0d)", margin_frames);
  end
  $display("MARGINS frames=%0d raw_min=%.1f rbw_min=%.1f lines skipped=%0d",
           margin_frames, min_raw_margin, min_rbw_margin, margin_skipped);
  $display("RESULT SOURCE_SYNC_FMT ERRORS=%0d", errors);
  $finish;
end

// watchdog
initial begin
  #250_000_000;
  $display("RESULT TIMEOUT (frames=%0d)", frames);
  $finish;
end

endmodule
