`timescale 1ns / 1ps
/*
 * Controller-level functional testbench for video_source_sync.v
 *
 * Drives the synchronizer exactly as video_formatter does: a raster
 * line counter that wraps on the module's registered per-line decision,
 * an asynchronous capture-domain anchor toggle (through the same
 * xpm_cdc_single the formatter uses), and live geometry inputs.  The
 * raster runs at four pixel clocks per line so entire PAL/NTSC frame
 * sequences with fractional-line cadences, arbitrary initial phases,
 * jitter, source loss, malformed rates, cadence switches, mode
 * reprogramming and interlace toggles are all simulated quickly.
 *
 * Checked invariants:
 *  - disabled: wrap is exactly nominal, never engaged, never hidden
 *  - enabled, before lock+warmup: picture hidden, frames nominal
 *  - locked+shown: wrap never before anchor+DELAY (phase guard) and,
 *    in both modes from the FIRST visible frame, never more than the
 *    bounded delay window past it; frame totals inside
 *    [min_last+1, 1152], delay near the 256-line guard, no recurring
 *    slips (long-run frame/anchor count match)
 *  - interlace-marked alternating field cadences (1123.2/1126.8 and
 *    1123.7/1127.3 lines, either field order) stay locked and visible
 *    with whole-line frame totals inside a 2-line spread around the
 *    field-pair average (raw per-anchor tracking modulates 4-5 lines),
 *    the anchor guard never below 256 lines, the average tracked with
 *    no ongoing slip, and source loss/reacquisition re-stabilizes
 *  - invalid/missing source: fall back to exact nominal free-running
 *    frames, hidden, bounded monitor sync, later reacquisition works
 *  - enable/mode/interlace transitions re-arm acquisition cleanly
 *
 * Plusargs: +SMOKE=1 shortens the steady-state anchor counts.
 */

module video_source_sync_tb;

localparam [11:0] LINE_LAST_PIXEL = 12'd3;  // last x counter value (clocks/line = this + 1)
localparam integer H_MAX = LINE_LAST_PIXEL; // 4 pixel clocks per raster line
localparam real LINE_NS = (H_MAX + 1) * 6.8;

// Production PAL centered geometry (video_scale.h / zz_video_modes.c):
// 1080-line canvas on a 1125-line nominal, vsync ending at vend=1089
// and active video through canvas+scale (1080+4), so the minimum legal
// wrap is line 1090 -> 1091-line acquisition frames, exactly like the
// native centered mode.
localparam integer NOMINAL_LAST = 1124;
localparam integer VSYNC_END = 1089;
localparam integer ACTIVE_END = 1084;
localparam integer MIN_TOTAL = VSYNC_END + 2;   // 1091
localparam integer MAX_TOTAL = 1152;
localparam integer DELAY_TARGET = 256;
localparam integer WIN_GUARD_LO = 254; // steady-window guard floor: 256 - CDC/sampling slack
localparam integer WIN_GUARD_HI = 262; // steady-window guard ceiling: 258 raw +2 clamp + slack

reg resetn = 0;
reg dvi_clk = 0;
always #3.4 dvi_clk = ~dvi_clk;

// live geometry inputs
reg [11:0] geom_last_line = NOMINAL_LAST;
reg [11:0] geom_vsync_end = VSYNC_END;
reg [12:0] geom_active_end = ACTIVE_END;
reg geom_interlace = 0;
reg enable = 0;

// asynchronous capture-domain anchor
reg anchor_async = 0;
wire anchor_pix;
xpm_cdc_single #(
  .DEST_SYNC_FF(3),
  .INIT_SYNC_FF(1),
  .SIM_ASSERT_CHK(0),
  .SRC_INPUT_REG(0)
) anchor_cdc_tb (
  .src_clk(1'b0),
  .src_in(anchor_async),
  .dest_clk(dvi_clk),
  .dest_out(anchor_pix)
);

// raster model, mirroring video_formatter's counters and wrap mux
reg [11:0] counter_y = 0;
reg [1:0] counter_x = 0;
integer line_index = 0;
wire line_advance = counter_x >= H_MAX;
wire line_uses_sync, line_is_last, video_hidden;
wire frame_wrap = line_uses_sync ? line_is_last
  : counter_y >= geom_last_line;

video_source_sync dut (
  .dvi_clk(dvi_clk),
  .resetn(resetn),
  .enable(enable),
  .anchor_toggle(anchor_pix),
  .nominal_last_line(geom_last_line),
  .vsync_end_line(geom_vsync_end),
  .active_end_line(geom_active_end),
  .interlace(geom_interlace),
  .raster_y(counter_y),
  .line_advance(line_advance),
  .line_last_pixel(LINE_LAST_PIXEL),
  .line_uses_sync(line_uses_sync),
  .line_is_last(line_is_last),
  .video_hidden(video_hidden)
);

always @(posedge dvi_clk) begin
  if (!resetn) begin
    counter_x <= 0;
    counter_y <= 0;
  end else if (line_advance) begin
    counter_x <= 0;
    line_index <= line_index + 1;
    if (frame_wrap) counter_y <= 0;
    else counter_y <= counter_y + 1'b1;
  end else begin
    counter_x <= counter_x + 1'b1;
  end
end

/* ------------------------------------------------------------------ */
/* Checks                                                             */
/* ------------------------------------------------------------------ */

integer errors = 0;
integer section_errors;
integer section_id = 0;

integer ref_valid = 0;
integer converged = 0;
integer anchor_fresh = 0;
integer anchors_at_prev_wrap = 0;
integer frames = 0;
integer last_wrap_index = 0;
integer frame_len;
integer delay_lines;
real last_anchor_pos = 0.0;
integer anchors_total = 0;
integer anchors_at_ref = 0;
integer frames_at_ref = 0;
integer i;
real rtmp;
reg line_tick_d;
reg shown_prev = 0;
integer last_geom_change_line = -1000000;
integer geom_quiet_frames;

integer cfg_smoke = 0;
integer steady_anchors = 40;

// geometry transitions make single mixed-decision frames legal; the
// exact-length checks are suspended for two frames after any change
always @(geom_last_line or geom_vsync_end or geom_active_end or
         geom_interlace)
  last_geom_change_line = line_index;

// steady-window recorder for the interlace cadence checks
integer measure_active = 0;
integer win_count = 0;
integer win_min = 0;
integer win_max = 0;
integer win_sum = 0;
integer win_violations = 0;
integer win_guard_min = 0;
integer win_guard_max = 0;
real    win_mean_target;
// coherent per-line samples for the observable blanking checks
reg enab_s = 0, enab_s_p = 0;
reg locked_s = 0, locked_s_p = 0;
reg pic_ok_s = 0, pic_ok_s_p = 0, pic_ok_s_pp = 0;
reg hidden_s = 0;
// per-line and per-frame checks, sampled mid-cycle after the edge
always @(negedge dvi_clk) begin
  if (resetn && line_tick_d) begin
    // coherent per-line samples for the observable blanking checks
    pic_ok_s_pp = pic_ok_s_p;
    pic_ok_s_p = pic_ok_s;
    enab_s_p = enab_s;
    locked_s_p = locked_s;
    enab_s = enable;
    locked_s = dut.locked;
    pic_ok_s = dut.picture_ok;
    hidden_s = video_hidden;
    geom_quiet_frames = (line_index - last_geom_change_line) / 1152;

    // while disabled the free-running compare must be used verbatim
    if (!enable) begin
      if (line_uses_sync) begin
        errors = errors + 1;
        if (errors < 40) $display("ERR %0d: sync engaged while disabled (line %0d)", section_id, line_index);
      end
      if (video_hidden) begin
        errors = errors + 1;
        if (errors < 40) $display("ERR %0d: hidden while disabled (line %0d)", section_id, line_index);
      end
    end else begin
      // observable blanking lifecycle (no internal mirror): the
      // picture may be unblanked only under lock held across two
      // consecutive line samples, and a blank must not persist while
      // the module reports a presented picture (sampled per line, so
      // no internal pipeline stage is pinned)
      if (enab_s_p && !locked_s_p && !locked_s && !hidden_s) begin
        errors = errors + 1;
        if (errors < 40) $display("ERR %0d: unblanked while unlocked (line %0d)", section_id, line_index);
      end
      if (enab_s && pic_ok_s && pic_ok_s_p && pic_ok_s_pp && hidden_s) begin
        errors = errors + 1;
        if (errors < 40) $display("ERR %0d: blank held while picture reported shown (line %0d)", section_id, line_index);
      end
      // engaged lines must never run past the adopted bounds
      if (line_uses_sync && dut.cfg_adopted && counter_y > dut.max_last_line) begin
        errors = errors + 1;
        if (errors < 40) $display("ERR %0d: raster past max_last (%0d > %0d)", section_id, counter_y, dut.max_last_line);
      end
    end

    if (counter_y == 0) begin
      // a frame just wrapped
      frames = frames + 1;
      anchor_fresh = anchors_total != anchors_at_prev_wrap;
      anchors_at_prev_wrap = anchors_total;
      frame_len = line_index - last_wrap_index;
      last_wrap_index = line_index;

      if (enable) begin
        if (line_uses_sync || dut.cfg_adopted) begin
          // frame total bounds whenever the sync path governed the frame
          if (frame_len < MIN_TOTAL || frame_len > MAX_TOTAL) begin
            errors = errors + 1;
            if (errors < 40) $display("ERR %0d: frame total %0d out of [%0d,%0d]", section_id, frame_len, MIN_TOTAL, MAX_TOTAL);
          end
          // unlocked (acquiring): exact nominal free-running totals
          if (!dut.locked && geom_quiet_frames >= 2 &&
              frame_len != dut.cfg_nominal_len) begin
            errors = errors + 1;
            if (errors < 40) $display("ERR %0d: unlocked frame total %0d != adopted nominal %0d", section_id, frame_len, dut.cfg_nominal_len);
          end
        end
        if (dut.picture_ok) begin
          rtmp = line_index;
          rtmp = rtmp - last_anchor_pos;
          delay_lines = rtmp;
          /* Upper phase bound from the FIRST visible frame in both
           * modes: a shown wrap must sit inside the bounded delay
           * window.  During late-phase acquisition the blanking floor
           * permits wraps hundreds of lines past the anchor (the
           * progressive frames the pre-fix bypass exposed), and this
           * rejects them in either mode. */
          if (delay_lines > WIN_GUARD_HI) begin
            errors = errors + 1;
            if (errors < 40) $display("ERR %0d: shown before safe read phase (%0d lines, lace=%0d)", section_id, delay_lines, geom_interlace);
          end
          if (delay_lines < DELAY_TARGET - 5) begin
            errors = errors + 1;
            if (errors < 40) $display("ERR %0d: shown frame delay %0d below guard floor %0d", section_id, delay_lines, DELAY_TARGET - 5);
          end
          /* acquisition may start with extra guard (delay above the
           * target); once settled at the target it must stay there.
           * Frames wrapped on a stale anchor (source just lost, the
           * watchdog has not fired yet) legitimately grow the delay. */
          if (anchor_fresh) begin
            if (!converged && delay_lines >= DELAY_TARGET - 2 &&
                delay_lines <= DELAY_TARGET + 8)
              converged = 1;
            else if (converged && delay_lines > DELAY_TARGET + 8) begin
              errors = errors + 1;
              if (errors < 40) $display("ERR %0d: converged phase slipped to %0d", section_id, delay_lines);
            end
          end
          if (!ref_valid) begin
            ref_valid = 1;
            frames_at_ref = frames;
            anchors_at_ref = anchors_total;
          end else if (frames - frames_at_ref > 4) begin
            i = (frames - frames_at_ref) - (anchors_total - anchors_at_ref);
            if (i > 2 || i < -2) begin
              errors = errors + 1;
              if (errors < 40) $display("ERR %0d: beat/drop %0d frames vs anchors over %0d frames", section_id, i, frames - frames_at_ref);
            end
          end
        end else begin
          // any non-shown frame invalidates the long-run reference
          ref_valid = 0;
          converged = 0;
        end
      end else begin
        ref_valid = 0;
        converged = 0;
      end

      // steady-window sampling: every wrap inside a window must be
      // locked, shown and unblanked with the anchor guard inside the
      // steady band; shown frames accumulate for the spread/mean
      if (measure_active) begin
        if (dut.picture_ok && dut.locked && !video_hidden) begin
          if (win_count == 0) begin
            win_min = frame_len;
            win_max = frame_len;
            win_guard_min = delay_lines;
            win_guard_max = delay_lines;
          end else begin
            if (frame_len < win_min) win_min = frame_len;
            if (frame_len > win_max) win_max = frame_len;
            if (delay_lines < win_guard_min) win_guard_min = delay_lines;
            if (delay_lines > win_guard_max) win_guard_max = delay_lines;
          end
          if (delay_lines < WIN_GUARD_LO || delay_lines > WIN_GUARD_HI) begin
            errors = errors + 1;
            if (errors < 40) $display("ERR %0d: steady guard %0d outside [%0d,%0d]", section_id, delay_lines, WIN_GUARD_LO, WIN_GUARD_HI);
          end
          win_sum = win_sum + frame_len;
          win_count = win_count + 1;
        end else begin
          errors = errors + 1;
          win_violations = win_violations + 1;
          if (errors < 40) $display("ERR %0d: steady-window frame not locked+shown (line %0d locked=%0d shown=%0d hidden=%0d)", section_id, line_index, dut.locked, dut.picture_ok, video_hidden);
        end
      end
    end
  end
end

// picture_ok may rise only at a frame wrap (it may fall anywhere:
// hiding on lost sync is immediate by design)
always @(negedge dvi_clk) begin
  if (resetn) begin
    if (!shown_prev && dut.picture_ok && !(line_tick_d && counter_y == 0)) begin
      errors = errors + 1;
      if (errors < 40) $display("ERR %0d: picture_ok rose mid-frame", section_id);
    end
    shown_prev = dut.picture_ok;
  end
end

always @(posedge dvi_clk) line_tick_d <= line_advance && resetn;

/* Anchor generator                                                   */
/* ------------------------------------------------------------------ */

integer seed = 32'h5EED_0001;

task send_anchors(input integer count, input real spacing);
  integer k;
  begin
    for (k = 0; k < count; k = k + 1) begin
      #(spacing * LINE_NS);
      anchor_async = ~anchor_async;
      last_anchor_pos = line_index;
      anchors_total = anchors_total + 1;
    end
  end
endtask

task send_anchors_jitter(input integer count, input real spacing,
                         input real amp);
  integer k;
  real jit;
  begin
    for (k = 0; k < count; k = k + 1) begin
      jit = amp * ((($random(seed) % 2000)) / 1000.0 - 1.0);
      #((spacing + jit) * LINE_NS);
      anchor_async = ~anchor_async;
      last_anchor_pos = line_index;
      anchors_total = anchors_total + 1;
    end
  end
endtask

task send_anchors_alternating(input integer count, input real spacing_a,
                              input real spacing_b);
  integer k;
  begin
    for (k = 0; k < count; k = k + 1) begin
      #((k % 2 == 0 ? spacing_a : spacing_b) * LINE_NS);
      anchor_async = ~anchor_async;
      last_anchor_pos = line_index;
      anchors_total = anchors_total + 1;
    end
  end
endtask

task wait_frames(input integer n);
  integer f0;
  begin
    f0 = frames;
    while (frames < f0 + n) @(posedge dvi_clk);
  end
endtask

task wait_lines(input integer n);
  integer l0;
  begin
    l0 = line_index;
    while (line_index < l0 + n) @(posedge dvi_clk);
  end
endtask

/* Bounded progressive acquisition: from the worst (latest) anchor
 * phase the 1091-line blanking floor ramps the wrap phase towards the
 * anchor by ~34 lines per frame against the 1125-line cadence, so the
 * picture cannot legally appear before ~35 anchors including lock and
 * warmup; 48 bounds the wait while still demanding that progressive
 * pixels actually reach the screen (passing while permanently hidden
 * is a failure, not a pass). */
task acquire_picture(input integer max_anchors, input real spacing);
  integer sent;
  begin
    sent = 0;
    while (!dut.picture_ok && sent < max_anchors) begin
      send_anchors(1, spacing);
      sent = sent + 1;
    end
    if (!dut.picture_ok) begin
      errors = errors + 1;
      $display("ERR %0d: not shown within %0d anchors", section_id, max_anchors);
    end
  end
endtask

// steady-window recorder control: frames wrapped while measure_active
// is high must stay locked/shown/unblanked inside the guard band, and
// the recorded frame lengths must meet the spread/mean verdicts
task window_begin(input real mean_target);
  begin
    win_count = 0;
    win_sum = 0;
    win_violations = 0;
    win_min = 0;
    win_max = 0;
    win_guard_min = 0;
    win_guard_max = 0;
    win_mean_target = mean_target;
    measure_active = 1;
  end
endtask

task window_end(input integer max_spread);
  real mean;
  real tol;
  begin
    measure_active = 0;
    mean = 0.0;
    if (win_count > 0) begin
      mean = win_sum * 1.0;
      mean = mean / win_count;
    end
    tol = (win_count >= 12) ? 0.3 : 0.6;
    $display("WINDOW %0d: %0d frames, len [%0d..%0d] spread %0d, mean %0.2f (target %0.1f), guard [%0d..%0d], %0d violations", section_id, win_count, win_min, win_max, win_max - win_min, mean, win_mean_target, win_guard_min, win_guard_max, win_violations);
    if (win_count < 3) begin
      errors = errors + 1;
      $display("ERR %0d: steady window too short (%0d frames)", section_id, win_count);
    end else begin
      if (win_max - win_min > max_spread) begin
        errors = errors + 1;
        $display("ERR %0d: steady frame spread %0d lines ([%0d..%0d]) exceeds %0d", section_id, win_max - win_min, win_min, win_max, max_spread);
      end
      if (mean > win_mean_target + tol || mean < win_mean_target - tol) begin
        errors = errors + 1;
        $display("ERR %0d: steady mean %0.2f misses cadence average %0.1f (tol %0.2f)", section_id, mean, win_mean_target, tol);
      end
    end
  end
endtask

task section(input integer id);
  begin
    section_id = id;
    converged = 0;
    section_errors = errors;
    $display("SECTION %0d start (line %0d, frames %0d)", id, line_index, frames);
  end
endtask

task section_done;
  begin
    if (errors != section_errors)
      $display("SECTION %0d FAIL (%0d new errors)", section_id, errors - section_errors);
    else
      $display("SECTION %0d PASS", section_id);
  end
endtask

/* ------------------------------------------------------------------ */
/* Sequence                                                           */
/* ------------------------------------------------------------------ */

integer phase;

initial begin
  if ($value$plusargs("SMOKE=%d", cfg_smoke)) ;
  if (cfg_smoke != 0) steady_anchors = 8;

  repeat (10) @(posedge dvi_clk);
  resetn = 1;
  wait_frames(1);

  // --- 1: disabled behaviour is exactly nominal free-running ---
  section(1);
  wait_frames(5);
  section_done;

  // --- 2: enable, clean PAL cadence, arbitrary (worst-case late) phase;
  // the bounded acquisition must still present progressive pixels ---
  section(2);
  enable = 1;
  acquire_picture(48, 1125.0);
  send_anchors(steady_anchors, 1125.0);
  if (!dut.picture_ok) begin
    errors = errors + 1;
    $display("ERR 2: lost picture during steady run");
  end
  section_done;

  // --- 3: re-acquisition from several initial phases ---
  for (phase = 0; phase < 3; phase = phase + 1) begin
    section(30 + phase);
    enable = 0;
    wait_frames(2);
    enable = 1;
    wait_lines(phase == 0 ? 17 : (phase == 1 ? 513 : 1101));
    acquire_picture(48, 1125.0);
    section_done;
  end

  // --- 4: jittered anchors ---
  section(4);
  send_anchors_jitter(6 + steady_anchors, 1125.0, 3.0);
  if (!dut.locked) begin
    errors = errors + 1;
    $display("ERR 4: jitter lost lock");
  end
  section_done;

  // --- 5: fractional drift (59.94-style +1.5 lines/frame) ---
  section(5);
  send_anchors(6 + steady_anchors, 1126.5);
  if (!dut.locked) begin
    errors = errors + 1;
    $display("ERR 5: fractional cadence lost lock");
  end
  section_done;

  // --- 6: interlaced half-line alternation, cadence stabilization ---
  // interlace marking re-arms acquisition; the alternating field
  // cadence must lock and present, then hold whole-line frame totals
  // inside a 2-line spread around the 1125.0-line field-pair average
  // (raw per-anchor tracking modulates 1123..1127, a 4-5 line spread)
  section(6);
  geom_interlace = 1;
  send_anchors_alternating(14, 1126.8, 1123.2);
  if (!dut.locked || !dut.picture_ok) begin
    errors = errors + 1;
    $display("ERR 6: no lock+warmup on interlaced cadence");
  end
  window_begin(1125.0);
  send_anchors_alternating(steady_anchors, 1126.8, 1123.2);
  window_end(2);
  // reversed field order (short field first): the stabilization must
  // not depend on which field carries the extra half line
  send_anchors_alternating(8, 1123.2, 1126.8);
  window_begin(1125.0);
  send_anchors_alternating(steady_anchors, 1123.2, 1126.8);
  window_end(2);
  // fractional field-pair average (1125.5): whole-line frames must
  // track the average rather than the 1125-line nominal
  send_anchors_alternating(8, 1123.7, 1127.3);
  window_begin(1125.5);
  send_anchors_alternating(steady_anchors, 1123.7, 1127.3);
  window_end(2);
  // source loss clears the cadence history: watchdog unlock+hide,
  // then the same interlaced cadence must re-stabilize
  wait_lines(1125 + 256 + 40);
  if (dut.locked) begin
    errors = errors + 1;
    $display("ERR 6: watchdog did not unlock after interlaced source loss");
  end
  if (!video_hidden) begin
    errors = errors + 1;
    $display("ERR 6: not hidden after interlaced source loss");
  end
  send_anchors_alternating(14, 1126.8, 1123.2);
  if (!dut.locked) begin
    errors = errors + 1;
    $display("ERR 6: no reacquisition on interlaced cadence");
  end
  // Lock acquisition does not imply stationary phase. After source loss
  // the raster can start anywhere in the field; the bounded wrap changes
  // its phase by at most the available blanking per frame.
  send_anchors_alternating(40, 1126.8, 1123.2);
  window_begin(1125.0);
  send_anchors_alternating(steady_anchors, 1126.8, 1123.2);
  window_end(2);
  // the remaining sections exercise progressive behaviour again
  geom_interlace = 0;
  section_done;

  // --- 7: sustained slow-but-valid cadence near the edge ---
  section(7);
  send_anchors(6 + steady_anchors, 1150.0);
  if (!dut.locked) begin
    errors = errors + 1;
    $display("ERR 7: valid slow cadence lost lock");
  end
  section_done;

  // --- 8: source loss, watchdog fallback, reacquisition ---
  section(8);
  wait_lines(1125 + 256 + 40);
  if (dut.locked) begin
    errors = errors + 1;
    $display("ERR 8: watchdog did not unlock");
  end
  if (!video_hidden) begin
    errors = errors + 1;
    $display("ERR 8: not hidden after watchdog");
  end
  acquire_picture(48, 1125.0);
  section_done;

  // --- 9: malformed fast cadence never locks ---
  section(9);
  enable = 0; wait_frames(2); enable = 1;
  send_anchors(12, 800.0);
  if (dut.locked || dut.picture_ok) begin
    errors = errors + 1;
    $display("ERR 9: locked on malformed fast cadence");
  end
  section_done;

  // --- 10: malformed slow cadence never locks ---
  section(10);
  enable = 0; wait_frames(2); enable = 1;
  send_anchors(6, 2000.0);
  if (dut.locked || dut.picture_ok) begin
    errors = errors + 1;
    $display("ERR 10: locked on malformed slow cadence");
  end
  section_done;

  // --- 11: erratic cadence never locks ---
  section(11);
  for (i = 0; i < 16; i = i + 1) begin
    rtmp = 400.0 + ($random(seed) % 1400);
    send_anchors(1, rtmp);
  end
  if (dut.locked || dut.picture_ok) begin
    errors = errors + 1;
    $display("ERR 11: locked on erratic cadence");
  end
  section_done;

  // --- 12: cadence switch away from the window unlocks ---
  section(12);
  enable = 0; wait_frames(2); enable = 1;
  acquire_picture(48, 1125.0);
  send_anchors(3, 1000.0);
  if (dut.locked || dut.picture_ok) begin
    errors = errors + 1;
    $display("ERR 12: stayed locked after cadence left window");
  end
  section_done;

  // --- 13: mode reprogram (nominal total change) re-arms ---
  section(13);
  enable = 0; wait_frames(2); enable = 1;
  acquire_picture(48, 1125.0);
  geom_last_line = 1149; // nominal 1150
  wait_frames(3);
  if (dut.cfg_last_line !== 12'd1149) begin
    errors = errors + 1;
    $display("ERR 13: new nominal not adopted (%0d)", dut.cfg_last_line);
  end
  acquire_picture(48, 1150.0);
  geom_last_line = NOMINAL_LAST;
  acquire_picture(48, 1125.0);
  section_done;

  // --- 14: interlace marking toggle re-arms ---
  section(14);
  acquire_picture(48, 1125.0);
  geom_interlace = 1;
  wait_frames(3);
  if (dut.locked) begin
    errors = errors + 1;
    $display("ERR 14: interlace toggle did not re-arm");
  end
  geom_interlace = 0;
  acquire_picture(48, 1125.0);
  section_done;

  // --- 15: mid-stream disable returns to exact nominal ---
  section(15);
  send_anchors(8, 1125.0);
  enable = 0;
  wait_lines(4);
  if (line_uses_sync) begin
    errors = errors + 1;
    $display("ERR 15: still engaged right after disable");
  end
  if (video_hidden) begin
    errors = errors + 1;
    $display("ERR 15: hidden while disabled");
  end
  wait_frames(4);
  enable = 1;
  acquire_picture(48, 1125.0);
  section_done;

  $display("RESULT SOURCE_SYNC_TB ERRORS=%0d", errors);
  $finish;
end

// watchdog
initial begin
  #50_000_000;
  $display("RESULT TIMEOUT (line %0d frames %0d)", line_index, frames);
  $finish;
end

endmodule
