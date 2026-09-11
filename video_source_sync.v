`timescale 1ns / 1ps
/*
 * MNT ZZ9000 Amiga Graphics and Coprocessor Card Firmware
 * Output source-frame synchronizer
 *
 * Copyright (C) 2026, MNT Research GmbH, Berlin
 *                    https://mntre.com
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * GNU General Public License v3.0 or later
 *
 * https://spdx.org/licenses/GPL-3.0-or-later.html
 *
 * Aligns the output raster's vertical frame boundary with the video
 * capture anchor emitted by videocap_sampler (cap_frame_anchor_toggle:
 * one toggle per captured input field/frame, at the first completed
 * post-crop full-width source row). Progressive output starts DELAY_LINES
 * after that anchor. Interlace uses a bounded field-pair phase estimate:
 * target = anchor + 258 lines + (previous_interval-current_interval)/4.
 * Pixel-clock interval measurements cancel the stationary odd/even
 * alternation without turning its sub-line phase into line-count noise.
 * The correction is limited to +/-2 lines, so every tracked interlace
 * frame retains at least the original 256-line capture/read guard.
 *
 * Phase guard derivation (1080-line native modes, nominal 1125 lines):
 * after the final source row is consumed (~output line 1046), the VDMA
 * prefetches the next frame's row zero from DDR; the formatter only
 * consumes that held start-of-frame at the vertical sync start line.
 * With the anchor DELAY_LINES=256 output lines before the frame wrap,
 * the worst-case read-after-write margin for that early prefetch is
 *
 *   margin = 1046 + DELAY_LINES - frame_total
 *
 * so the total frame length must stay within
 *
 *   MAX_TOTAL_LINES = 1152  ->  >= ~140 output lines of margin,
 *
 * which also covers a capture writeback of up to one input line
 * (~3.6 output lines at PAL rates) many times over.  This is a
 * precondition of the single-frame-buffer design, not an unconditional
 * guarantee: capture writeback latency must stay within one input line
 * under the supported memory bandwidth, or a torn frame is possible.
 * When acquisition starts from a late anchor phase, the first tracked
 * frames wrap at the blanking floor or maximum hundreds of lines past
 * their anchor; the shown-picture gate rejects those wraps (max-forced
 * ones, and any wrap whose anchor age has left the bounded delay
 * window), so the picture stays hidden until the phase ramps into the
 * delay guard and every displayed frame carries its full margin.
 * The bound constants below permit at most 27 lines of extension past
 * the 1125-line nominal (1125+27 = 1152) and accept only anchor
 * cadences within +/-27 lines of nominal, which real 50/59.94 Hz
 * sources with crystal jitter and interlace half-line offsets meet
 * with a wide margin, while malformed rates are rejected and fall
 * back to exact nominal free-running sync.
 * Mechanism: the last raster line of each output frame is decided once,
 * at the start of that line, and held for the whole line.  The parent
 * formatter muxes this registered decision in place of its free-running
 * `counter_y >= vga_v_max` compare, so the line-bank prefetch computed
 * at hmax-1 and the actual counter_y wrap always see the SAME decision.
 * While enabled, the wrap may happen only inside vertical blanking:
 * never before both vertical sync and active video finish, and never
 * beyond min(nominal_last+27, 1151). Progressive tracking uses the latest
 * raw anchor; interlace uses the field-pair target above. It follows the
 * average source cadence, not a hard-coded refresh rate. Pixel and raster
 * quantization can still move an output boundary by a line. Anything not
 * provably safe (unlocked, fresh cadence, geometry in transition) falls
 * back to nominal free-running frames with the picture blanked.
 *
 * Timing structure (the 150 MHz pixel-clock domain has a 6.667 ns
 * budget): every per-line decision path in this module is a pure
 * register-to-register compare.  All arithmetic (the +1/+27 frame
 * lengths, the min/max wrap bounds, the +/-27 acceptance window, the
 * watchdog limit) is evaluated against per-frame constants and staged
 * through a small bound pipeline at the frame wrap: stage A latches the
 * adopted geometry at the wrap, stage B resolves the raw bounds one
 * line later, stage C subtracts the raster-threshold offset and marks
 * the bounds valid on the line after that.  Frames are at least 512
 * lines and the earliest legal wrap is past vertical sync, so the
 * two-line pipeline delay is invisible to the wrap decisions; while
 * bounds are not yet valid (or a live geometry change invalidated them)
 * the module stays disengaged and the parent's nominal compare governs.
 *
 * All state runs in the pixel clock domain.  enable and the anchor
 * toggle are already CDC-synchronized by the parent formatter; the
 * geometry inputs are the formatter's continuously re-registered dvi
 * domain copies, watched here through two delayed copies so the change
 * detector itself is a registered-to-registered compare.
*/

module video_source_sync(
  // pixel clock domain
  input wire        dvi_clk,
  input wire        resetn,

  // control (dvi_clk domain, quasi-static, CDC'd by the parent)
  input wire        enable,
  // capture-domain anchor toggle, CDC'd to dvi_clk by the parent
  input wire        anchor_toggle,

  // live output geometry (dvi_clk domain copies of control registers)
  input wire [11:0] nominal_last_line, // counter_y value of the nominal frame's last line
  input wire [11:0] vsync_end_line,    // first line after vertical sync
  input wire [12:0] active_end_line,   // first line after active video (incl. scale factor)
  input wire [11:0] line_last_pixel,  // clocks per output line minus one
  input wire        interlace,         // quasi-static control_interlace

  // raster position (parent counters)
  input wire [11:0] raster_y,
  input wire        line_advance,      // asserted on the final dvi_clk cycle of each line

  // registered at each line start, valid for the whole line
  output reg        line_uses_sync,    // this line's wrap follows line_is_last
  output reg        line_is_last,      // this line is the last of the output frame
  output reg        video_hidden,      // blank the picture (enabled but not locked/warm)

  // observational diagnostics, dvi_clk domain: coherent read-only mirror
  // of the functional state (bit map documented at the bottom of this
  // file); never feeds any functional decision
  output wire [63:0] diagnostic_data
);

localparam [15:0] DELAY_LINES     = 16'd256;  // anchor -> frame start target, output lines
localparam [15:0] EXT_LINES       = 16'd27;   // bounded extension past nominal last line
localparam [15:0] MAX_TOTAL_LINES = 16'd1152; // absolute frame-length ceiling (see header)
localparam [15:0] PERIOD_TOL      = 16'd27;   // anchor interval acceptance around nominal
localparam [3:0]  LOCK_INTERVALS  = 4'd4;     // consecutive valid intervals before lock
localparam [15:0] WATCHDOG_LINES  = 16'd256;  // lines past a nominal period without an anchor
localparam [15:0] MIN_FRAME_LINES = 16'd512;  // engagement floor: 2*DELAY_LINES
localparam [15:0] MAX_FRAME_LINES = 16'd4031; // engagement ceiling: +EXT_LINES+1 <= 4096

/* ------------------------------------------------------------------ */
/* Geometry watching (two delayed copies; register-to-register)         */
/* ------------------------------------------------------------------ */

reg [11:0] watch1_last, watch2_last;
reg [11:0] watch1_pixel, watch2_pixel;
reg [11:0] watch1_vsync, watch2_vsync;
reg [12:0] watch1_active, watch2_active;
reg        watch1_interlace, watch2_interlace;
reg        cfg_changed_d1;

wire cfg_changed = watch1_last != watch2_last ||
                   watch1_pixel != watch2_pixel ||
                   watch1_vsync != watch2_vsync ||
                   watch1_active != watch2_active ||
                   watch1_interlace != watch2_interlace;
wire cfg_quiet = !cfg_changed && !cfg_changed_d1;

/* Frame-length validity of the live nominal as two constant compares
 * (nominal+1 in [MIN_FRAME_LINES, MAX_FRAME_LINES]).  No adder: this
 * gates the per-line engagement path. */
wire nominal_valid = nominal_last_line >= 12'd511 &&
                     nominal_last_line <= 12'd4030 &&
                     vsync_end_line >= 12'd1 &&
                     vsync_end_line < nominal_last_line;
wire cfg_adopt_now = cfg_quiet && nominal_valid;


/* ------------------------------------------------------------------ */
/* Stage A: adopted geometry (latched at the frame wrap)               */
/* ------------------------------------------------------------------ */

/* Raw copies exist only for the live-vs-adopted re-arm compare; the
 * derived values feed the bound pipeline. */
reg [11:0] cfg_last_line;
reg [11:0] cfg_last_pixel;
reg [12:0] cfg_line_clocks;
reg [11:0] cfg_vsync_end;
reg [12:0] cfg_active_end;
reg        cfg_interlace;
reg [15:0] cfg_vsync_floor;   // vsync_end + 1
reg [15:0] cfg_nominal_len;   // nominal_last + 1
reg [15:0] cfg_ext_max;       // nominal_last + EXT_LINES
reg        cfg_adopted;

/* Re-arm (drop lock and hide) when the frame boundary adopts a changed
 * geometry or interlace marking, or cannot adopt a settled set. */
wire cfg_changed_vs_adopted = !cfg_adopt_now ||
  nominal_last_line != cfg_last_line ||
  line_last_pixel != cfg_last_pixel ||
  vsync_end_line != cfg_vsync_end ||
  active_end_line != cfg_active_end ||
  interlace != cfg_interlace;

wire [15:0] cfg_active_floor_w = {3'b0, cfg_active_end};
wire [15:0] nominal_line = {4'b0, cfg_last_line};
wire [15:0] floor_max_a =
  cfg_vsync_floor > cfg_active_floor_w ? cfg_vsync_floor : cfg_active_floor_w;
/* The wrap floor is vertical-blank safety only (vsync completed, active
 * video done).  It must NOT include the nominal length: tracking may
 * only ever extend a frame up to the max bound, so a nominal floor
 * makes the 256-line delay equilibrium one-sided — once the anchor
 * phase dips below nominal-DELAY (jitter does this at matching rates),
 * wraps pin at nominal, the phase free-runs, and the guard erodes
 * silently.  With the blank-only floor the equilibrium is stable from
 * both sides for every accepted cadence. */
wire [15:0] bound_min_candidate = floor_max_a;
wire [15:0] bound_max_candidate =
  cfg_ext_max > MAX_TOTAL_LINES - 16'd1 ? MAX_TOTAL_LINES - 16'd1 : cfg_ext_max;

/* ------------------------------------------------------------------ */
/* Stages B/C: per-frame bound pipeline                                */
/* ------------------------------------------------------------------ */

reg [15:0] bound_min_raw, bound_max_raw;
reg [15:0] period_lo, period_hi, watchdog_limit;
reg        bounds_valid;
reg [1:0]  bounds_pipe; // adopt pulse walks B (bit1) then C (bit0)

/* Final raster thresholds: the decision for the upcoming line marks
 * that line last when raster_y >= threshold, i.e. exactly when the
 * marked line number reaches the original bound. */
reg [15:0] wrap_min_thr, wrap_max_thr, unlock_thr;

/* Exported for the regression testbench's engaged-bound check only. */
wire [15:0] max_last_line = wrap_max_thr + 16'd1;

/* ------------------------------------------------------------------ */
/* Anchor tracking and lock validation                                 */
/* ------------------------------------------------------------------ */

reg anchor_q;
wire anchor_event = anchor_toggle != anchor_q;

reg [15:0] interval_ctr;      // output lines since the last anchor event
reg [3:0]  good_intervals;    // consecutive plausible anchor intervals
reg        locked;
reg [1:0]  warm_frames;       // complete frames tracked under lock
reg        picture_ok;
reg        last_line_forced; // the frame's wrap line was max-forced (short guard)
reg [15:0] lines_since_anchor;

wire interval_plausible = cfg_adopted && bounds_valid &&
  interval_ctr >= period_lo && interval_ctr <= period_hi;

reg enable_q;
wire enable_rise = enable && !enable_q;

/* Wrap decision inputs for the next raster line (raster_y + 1), in pure
 * compare form: lines_since_anchor reaches DELAY_LINES-2 at the last
 * clock of raster_y exactly when its end-of-next-line value
 * (lines_since_anchor+2, or 2 right at an anchor) reaches DELAY_LINES. */
/* The interlace threshold is prepared well before the guard expires.
 * At the last pixel of a line, the parent's next line ends one complete
 * line later. anchor_pixel_age excludes the current edge, hence the
 * threshold (258*line_clocks + correction) - line_clocks - 1. */
reg [24:0] anchor_pixel_age;
reg [20:0] lace_base_threshold;
reg [13:0] lace_correction_limit;
reg [20:0] lace_delay_threshold;
reg [20:0] lace_guard_floor, lace_guard_ceiling;
reg [24:0] lace_interval_pixels, lace_previous_pixels;
reg lace_interval_valid;
reg [2:0] lace_pipe;
reg signed [25:0] lace_interval_delta;
wire signed [25:0] lace_quarter_delta = lace_interval_delta >>> 2;
wire signed [25:0] lace_limit_signed = $signed({12'b0, lace_correction_limit});
reg signed [14:0] lace_correction;

wire lsa_delay_met = !anchor_event &&
  (cfg_interlace
    ? anchor_pixel_age >= {4'b0, lace_delay_threshold}
    : lines_since_anchor >= DELAY_LINES - 16'd2);
/* Lock validates source cadence, not raster phase. During acquisition
 * the bounded wrap can still be hundreds of lines late: exposing that
 * frame would let capture overwrite a woven/source row before scanout
 * reads it.  Admit interlace pixels only at the settled 256..261-line
 * phase window (the 260-line corrected target plus at most one
 * raster-line rounding); admit progressive pixels only inside the
 * bounded anchor-age window below. */
wire lace_picture_safe = !anchor_event &&
  anchor_pixel_age >= {4'b0, lace_guard_floor} &&
  anchor_pixel_age <= {4'b0, lace_guard_ceiling};

/* Progressive input skips the field-pair estimator, not the
 * capture/read guard: only unhide a frame whose wrap stayed inside
 * the bounded anchor delay.  From a late anchor phase the wrap floor
 * (past vertical sync/active end) can sit hundreds of lines past the
 * anchor; unhiding there would let the next capture overwrite lower
 * source rows before the x4 scanout requests them, so such frames
 * stay hidden until the phase ramps down into the delay window.  The
 * per-line decision releases the wrap at a DELAY_LINES anchor age
 * (decision threshold DELAY_LINES-2 plus the decision-to-wrap line),
 * so the DELAY_LINES+1 bound just covers that rounding.  An anchor
 * landing exactly on the wrap edge invalidates the age (the counter
 * clears only on that edge), so that frame hides as well. */
wire prog_picture_safe = !anchor_event &&
  lines_since_anchor <= DELAY_LINES + 16'd1;

/* Consecutive intervals share their middle anchor: their sum spans one
 * full interlaced frame. Correcting the current anchor by one quarter of
 * their difference places both fields on the same averaged phase grid.
 * Clamp abrupt/nonstationary changes rather than spending the DDR guard.
 * Separate capture/subtract/clamp/add stages keep arithmetic off the
 * once-per-line wrap decision path at the 150 MHz runtime pixel clock. */
always @(posedge dvi_clk) begin
  if (!resetn) begin
    anchor_pixel_age <= 25'd0;
    lace_interval_pixels <= 25'd0;
    lace_previous_pixels <= 25'd0;
    lace_interval_valid <= 1'b0;
    lace_pipe <= 3'b0;
    lace_interval_delta <= 26'sd0;
    lace_correction <= 15'sd0;
    lace_delay_threshold <= 21'd0;
  end else begin
    if (anchor_event)
      anchor_pixel_age <= 25'd0;
    else if (!(&anchor_pixel_age))
      anchor_pixel_age <= anchor_pixel_age + 25'd1;

    if (!enable || enable_rise || cfg_changed_d1 || !cfg_adopted ||
        !cfg_interlace || (anchor_event && !interval_plausible) ||
        (locked && lines_since_anchor > watchdog_limit)) begin
      lace_interval_valid <= 1'b0;
      lace_pipe <= 3'b0;
      lace_delay_threshold <= lace_base_threshold;
    end else begin
      lace_pipe <= {lace_pipe[1:0], 1'b0};
      if (anchor_event) begin
        lace_interval_pixels <= anchor_pixel_age + 25'd1;
        lace_previous_pixels <= lace_interval_pixels;
        lace_interval_valid <= 1'b1;
        lace_pipe[0] <= lace_interval_valid;
      end
      if (lace_pipe[0])
        lace_interval_delta <= $signed({1'b0, lace_previous_pixels}) -
                               $signed({1'b0, lace_interval_pixels});
      if (lace_pipe[1]) begin
        if (lace_quarter_delta > lace_limit_signed)
          lace_correction <= $signed({1'b0, lace_correction_limit});
        else if (lace_quarter_delta < -lace_limit_signed)
          lace_correction <= -$signed({1'b0, lace_correction_limit});
        else
          lace_correction <= lace_quarter_delta[14:0];
      end
      if (lace_pipe[2])
        lace_delay_threshold <= lace_base_threshold +
          {{6{lace_correction[14]}}, lace_correction};
    end
  end
end

always @(posedge dvi_clk) begin
  if (!resetn) begin
    watch1_last <= 12'd0;
    watch1_pixel <= 12'd0;
    watch1_vsync <= 12'd0;
    watch1_active <= 13'd0;
    watch1_interlace <= 1'b0;
    watch2_last <= 12'd0;
    watch2_pixel <= 12'd0;
    watch2_vsync <= 12'd0;
    watch2_active <= 13'd0;
    watch2_interlace <= 1'b0;
    cfg_changed_d1 <= 1'b0;
    cfg_last_line <= 12'd0;
    cfg_last_pixel <= 12'd0;
    cfg_line_clocks <= 13'd0;
    lace_base_threshold <= 21'd0;
    lace_correction_limit <= 14'd0;
    lace_guard_floor <= 21'd0;
    lace_guard_ceiling <= 21'd0;
    cfg_vsync_end <= 12'd0;
    cfg_active_end <= 13'd0;
    cfg_interlace <= 1'b0;
    cfg_vsync_floor <= 16'd0;
    cfg_nominal_len <= 16'd0;
    cfg_ext_max <= 16'd0;
    cfg_adopted <= 1'b0;
    bound_min_raw <= 16'd0;
    bound_max_raw <= 16'd0;
    period_lo <= 16'd0;
    period_hi <= 16'd0;
    watchdog_limit <= 16'd0;
    bounds_valid <= 1'b0;
    bounds_pipe <= 2'b00;
    wrap_min_thr <= 16'd0;
    wrap_max_thr <= 16'd0;
    unlock_thr <= 16'd0;
    anchor_q <= 1'b0;
    enable_q <= 1'b0;
    interval_ctr <= 16'd0;
    good_intervals <= 4'd0;
    locked <= 1'b0;
    warm_frames <= 2'd0;
    picture_ok <= 1'b0;
    last_line_forced <= 1'b0;
    lines_since_anchor <= 16'd0;
    line_uses_sync <= 1'b0;
    line_is_last <= 1'b0;
    video_hidden <= 1'b0;
  end else begin
    /* continuous watchers */
    watch2_last <= watch1_last;
    watch2_pixel <= watch1_pixel;
    watch2_vsync <= watch1_vsync;
    watch2_active <= watch1_active;
    watch2_interlace <= watch1_interlace;
    watch1_last <= nominal_last_line;
    watch1_pixel <= line_last_pixel;
    watch1_vsync <= vsync_end_line;
    watch1_active <= active_end_line;
    watch1_interlace <= interlace;
    cfg_changed_d1 <= cfg_changed;
    anchor_q <= anchor_toggle;
    enable_q <= enable;
    video_hidden <= enable && !picture_ok;

    /* Per-line decisions, registered on the final cycle of the previous
     * line so every consumer in the parent sees one stable value for
     * the whole line (including the hmax-1 prefetch computation).
     * Engagement needs adopted geometry AND resolved bounds; a live
     * geometry change drops it for the following line immediately. */
    if (line_advance) begin
      if (!anchor_event) begin
        interval_ctr <= interval_ctr + 16'd1;
        lines_since_anchor <= lines_since_anchor + 16'd1;
      end

      if (line_uses_sync ? line_is_last : raster_y >= nominal_last_line) begin
        /* Frame wrap: adopt the geometry for the incoming frame. */
        if (cfg_adopt_now) begin
          cfg_last_line <= nominal_last_line;
          cfg_last_pixel <= line_last_pixel;
          cfg_line_clocks <= {1'b0, line_last_pixel} + 13'd1;
          cfg_vsync_end <= vsync_end_line;
          cfg_active_end <= active_end_line;
          cfg_interlace <= interlace;
          cfg_vsync_floor <= {4'b0, vsync_end_line} + 16'd1;
          cfg_nominal_len <= {4'b0, nominal_last_line} + 16'd1;
          cfg_ext_max <= {4'b0, nominal_last_line} + EXT_LINES;
          cfg_adopted <= 1'b1;
        end else begin
          cfg_adopted <= 1'b0;
        end
        bounds_pipe <= cfg_adopt_now ? 2'b10 : 2'b00;

        if (cfg_changed_vs_adopted) begin
          /* Mode or interlace transition: re-arm from a clean state. */
          locked <= 1'b0;
          good_intervals <= 4'd0;
          warm_frames <= 2'd0;
          picture_ok <= 1'b0;
        end else begin
          if (locked && warm_frames != 2'd3)
            warm_frames <= warm_frames + 2'd1;
          /* Unhide only at a frame boundary, after one complete frame
           * was tracked under lock (warm_frames sampled pre-update),
           * and only on a wrap that kept the anchor phase inside the
           * mode's safe window: interlace uses the corrected
           * field-pair pixel window, progressive the bounded line
           * age. */
          picture_ok <= locked && warm_frames != 2'd0 &&
                        !last_line_forced &&
                        (cfg_interlace ? lace_picture_safe
                                       : prog_picture_safe);
        end

        /* The incoming line zero can never be a legal wrap line (the
         * floor is past vertical sync), so the parent's nominal compare
         * governs it; both decision outputs are idle for that line. */
        line_uses_sync <= 1'b0;
        line_is_last <= 1'b0;
      end else begin
        /* Bound pipeline: B resolves raw min/max from stage A, C folds
         * the raster-threshold offset and publishes validity. */
        if (bounds_pipe[1]) begin
          bound_min_raw <= bound_min_candidate;
          bound_max_raw <= bound_max_candidate;
          period_lo <= cfg_nominal_len - PERIOD_TOL;
          period_hi <= cfg_nominal_len + PERIOD_TOL;
          watchdog_limit <= cfg_nominal_len + WATCHDOG_LINES;
          lace_base_threshold <= {cfg_line_clocks, 8'b0} +
                                 {9'b0, cfg_last_pixel};
          lace_correction_limit <= {cfg_line_clocks, 1'b0};
          lace_guard_floor <= {cfg_line_clocks, 8'b0} - 21'd1;
        end
        if (bounds_pipe[0]) begin
          wrap_min_thr <= bound_min_raw - 16'd1;
          wrap_max_thr <= bound_max_raw - 16'd1;
          unlock_thr <= nominal_line - 16'd1;
          bounds_valid <= 1'b1;
          lace_guard_ceiling <= lace_base_threshold +
                                {6'b0, cfg_line_clocks, 2'b0};
        end
        bounds_pipe <= {1'b0, bounds_pipe[1]};

        /* Decision for the upcoming raster line: pure compares. */
        line_uses_sync <= enable && cfg_adopted && !cfg_changed_d1 &&
                          bounds_valid;
        line_is_last <= !locked
          ? (raster_y >= unlock_thr)
          : ((raster_y >= wrap_min_thr && lsa_delay_met) ||
             raster_y >= wrap_max_thr);
        /* A wrap forced by the bounded maximum is safe but carries a
         * shorter guard while the anchor phase ramps towards the full
         * delay; such frames keep the picture hidden. */
        last_line_forced <= locked && enable && bounds_valid &&
          !cfg_changed_d1 && raster_y >= wrap_max_thr && !lsa_delay_met;
      end
    end

    /* A live geometry change invalidates the resolved bounds; the next
     * frame boundary re-adopts and re-runs the pipeline. */
    if (cfg_changed_d1) begin
      bounds_valid <= 1'b0;
      bounds_pipe <= 2'b00;
    end

    /* Enable transitions always restart acquisition cleanly. */
    if (!enable || enable_rise) begin
      locked <= 1'b0;
      good_intervals <= 4'd0;
      warm_frames <= 2'd0;
      picture_ok <= 1'b0;
    end

    /* Source loss: bounded wait, then nominal free-running fallback. */
    if (locked && lines_since_anchor > watchdog_limit) begin
      locked <= 1'b0;
      good_intervals <= 4'd0;
      warm_frames <= 2'd0;
      picture_ok <= 1'b0;
    end

    /* Anchor event: highest priority, overrides the line counters. */
    if (anchor_event) begin
      interval_ctr <= 16'd0;
      lines_since_anchor <= 16'd0;
      if (interval_plausible) begin
        if (good_intervals != LOCK_INTERVALS)
          good_intervals <= good_intervals + 4'd1;
        if (good_intervals + 4'd1 >= LOCK_INTERVALS && !locked) begin
          locked <= 1'b1;
          warm_frames <= 2'd0;
          picture_ok <= 1'b0;
        end
      end else begin
        good_intervals <= 4'd0;
        locked <= 1'b0;
        warm_frames <= 2'd0;
        picture_ok <= 1'b0;
      end
    end
  end
end

/* ------------------------------------------------------------------ */
/* Observational diagnostics (read-only mirror, no functional fanout)  */
/* ------------------------------------------------------------------ */

/* The block below is an independent observer: it samples the same
 * registered values the functional block above bases its decisions on,
 * and nothing it computes ever feeds back into that block.  It
 * replicates the functional frame-wrap predicate (the compare at the
 * top of the line_advance section) so the reported frame metrics line
 * up exactly with the frames the parent raster actually produced. */
wire diag_frame_wrap = line_advance &&
  (line_uses_sync ? line_is_last : raster_y >= nominal_last_line);

/* Mirror the functional source-loss predicate; the diagnostic sticky
 * bit below records it only while source synchronization is enabled. */
wire diag_watchdog_now = locked && lines_since_anchor > watchdog_limit;

/* Event counters and the sticky watchdog bit restart on reset or an
 * enable rise and accumulate only while enabled.  The interval and
 * frame metrics keep updating while disabled as a free-running
 * reference (the raster and the anchor toggle keep running then).
 *
 * PRE-EDGE CONVENTION: every sample below reads the value the
 * functional block saw on that same clock edge, before that edge's
 * own update applies.  In particular diag_wrap_age is the anchor age
 * AT the wrap line, before the wrap-edge increment of
 * lines_since_anchor, and diag_interval is the completed interval
 * count at the anchor event, before interval_ctr is cleared. */
reg [15:0] diag_interval;       // last completed anchor interval (lines)
reg [15:0] diag_wrap_age;       // anchor age at the last frame wrap (lines)
reg [11:0] diag_frame_total;    // raster_y+1 at the last frame wrap
reg [3:0]  diag_rejected;       // implausible anchor intervals, saturating
reg [3:0]  diag_cfg_changes;    // cfg_changed_d1 cycles, saturating
reg [3:0]  diag_anchor_mod16;   // anchor events, modulo 16
reg        diag_wrap_forced;    // last wrap was max-forced (short guard)
reg        diag_watchdog_fired; // sticky: source watchdog tripped

always @(posedge dvi_clk) begin
  if (!resetn) begin
    diag_interval <= 16'd0;
    diag_wrap_age <= 16'd0;
    diag_frame_total <= 12'd0;
    diag_wrap_forced <= 1'b0;
    diag_rejected <= 4'd0;
    diag_cfg_changes <= 4'd0;
    diag_anchor_mod16 <= 4'd0;
    diag_watchdog_fired <= 1'b0;
  end else begin
    /* Frame metrics: free-running reference, sampled on every wrap
     * regardless of enable (also on the enable_rise cycle itself).
     * diag_frame_total is 12-bit modulo: it can only wrap past 4095,
     * far above any legal geometry (real native frames <= 1152). */
    if (diag_frame_wrap) begin
      diag_wrap_age <= lines_since_anchor;   // pre-edge, see above
      diag_frame_total <= raster_y + 12'd1;
      /* The exact stale/valid value the functional picture_ok gate
       * evaluated for this same wrap. */
      diag_wrap_forced <= last_line_forced;
    end

    if (anchor_event)
      diag_interval <= interval_ctr;         // pre-edge, see above

    /* Enable rise starts a new observation epoch. Coincident events
     * are not counted on that reset edge. */
    if (enable_rise) begin
      diag_rejected <= 4'd0;
      diag_cfg_changes <= 4'd0;
      diag_anchor_mod16 <= 4'd0;
      diag_watchdog_fired <= 1'b0;
    end else begin
      /* Raw live geometry/interlace change events (not only the
       * re-arm at the frame wrap); cfg_changed_d1 is high for one
       * dvi_clk cycle per detected change. */
      if (enable && cfg_changed_d1 && diag_cfg_changes != 4'd15)
        diag_cfg_changes <= diag_cfg_changes + 4'd1;

      if (enable && diag_watchdog_now)
        diag_watchdog_fired <= 1'b1;

      if (anchor_event && enable) begin
        diag_anchor_mod16 <= diag_anchor_mod16 + 4'd1;
        if (!interval_plausible && diag_rejected != 4'd15)
          diag_rejected <= diag_rejected + 4'd1;
      end
    end
  end
end

/* Contract bit map (dvi_clk-domain samples; the parent formatter moves
 * the whole bus coherently into the m_axis_vid_aclk domain):
 *   [63:48] last completed anchor interval, output lines
 *   [47:32] anchor age at the last frame wrap, output lines (pre-edge)
 *   [31:20] last completed raster frame total (raster_y+1 at wrap)
 *   [19:16] rejected (implausible) anchor interval count, saturates at 15
 *   [15:12] live cfg_changed_d1 cycle count, saturates at 15
 *   [11:8]  anchor event count, modulo 16
 *   [7]     most recent wrap was max-forced
 *   [6]     current raw interlace input
 *   [5]     sticky: source watchdog fired since enable rise
 *   [4]     bounds_valid
 *   [3]     video_hidden
 *   [2]     picture_ok
 *   [1]     locked
 *   [0]     enable */
assign diagnostic_data = {
  diag_interval,       // [63:48]
  diag_wrap_age,       // [47:32]
  diag_frame_total,    // [31:20]
  diag_rejected,       // [19:16]
  diag_cfg_changes,    // [15:12]
  diag_anchor_mod16,   // [11:8]
  diag_wrap_forced,    // [7]
  interlace,           // [6]
  diag_watchdog_fired, // [5]
  bounds_valid,        // [4]
  video_hidden,        // [3]
  picture_ok,          // [2]
  locked,              // [1]
  enable               // [0]
};

endmodule
