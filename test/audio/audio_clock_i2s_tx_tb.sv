`timescale 1ns / 1ps

/*
 * Compare the production I2S transmitter driven by audio_clock's
 * regenerated LRCLK with an otherwise identical transmitter wired directly
 * to an ADAU1701 model whose boot behaviour matches the firmware: absent
 * clocks, static LRCLK, a wrong serial format before the TDM8
 * configuration, mid-stream codec resets with a phase shift, and single
 * glitch pulses.  Pad insertion skew is swept around the nominal edge so
 * the sample-edge margins are exercised at both tSODM corners.
 */
module audio_clock_i2s_tx_tb;
  localparam integer REQUIRED_FRAMES = 32;
  localparam real BCLK_HALF_NS = 40.690104;
  // Models the codec pad through the FPGA input path (IBUF plus the kept
  // regenerator input LUTs) against the BCLK input's own insertion delay.
  localparam real LRCLK_INPUT_DELAY_NS = 0.5;
  // Models the kept regenerator output LUTs against clkgen's capture
  // instant when LRCLK changes on the same BCLK edge that raises sclk.
  localparam real REGEN_OUTPUT_DELAY_NS = 0.5;

  reg fclk_in = 1'b0;
  reg bridge_resetn = 1'b0;
  reg axi_clk = 1'b0;
  reg axi_resetn = 1'b0;
  reg axi_awvalid = 1'b0;
  wire axi_awready;
  reg [7:0] axi_awaddr = 8'b0;
  reg axi_wvalid = 1'b0;
  wire axi_wready;
  reg [31:0] axi_wdata = 32'b0;
  wire axi_bvalid;
  reg axi_bready = 1'b0;
  wire [1:0] axi_bresp;
  reg tx_valid = 1'b0;
  reg [31:0] tx_data = 32'ha55a_3cc3;
  reg [2:0] tx_tid = 3'b0;
  wire tx_ready;
  wire reference_ready;

  // Codec model
  reg codec_run = 1'b0;
  reg codec_tdm = 1'b0;
  reg codec_bclk = 1'b0;
  reg codec_lrclk = 1'b1;
  integer codec_half = 128;
  integer codec_count = 0;
  integer codec_falls = 0;
  real lrclk_delay_ns;
  real bclk_skew_ns;
  real bclk_delay_ns_used;
  real lrclk_extra_delay_ns;

  reg dut_bclk = 1'b0;
  reg dut_lrclk = 1'b1;
  reg regen_lrclk_delayed = 1'b1;

  wire divided_bclk;
  wire bridge_mclk;
  wire regenerated_lrclk;
  wire regenerated_sdata;
  wire reference_sdata;

  integer compared_frames = 0;
  integer compared_bits = 0;
  integer regen_words = 0;
  reg check_window = 1'b0;
  reg compare_armed = 1'b0;
  reg compare_sdata = 1'b1;
  reg disconnect_tx_sclk = 1'b0;
  wire regenerated_sclk = disconnect_tx_sclk ? 1'b0 : divided_bclk;
  integer glitch_guard_frames = 0;
  reg previous_lrclk = 1'b1;
  reg glitch_held_lrclk = 1'b1;
  integer words_mark;
  audio_clock bridge (
    .bclk_in(dut_bclk), .fclk_in(fclk_in), .lrclk_in(dut_lrclk),
    .resetn(bridge_resetn), .sdata_in(1'b0), .bclk_out(divided_bclk),
    .mclk_out(bridge_mclk), .tx_lrclk_out(regenerated_lrclk),
    .rx_lrclk_out(), .rx_sdata_out()
  );

  zz9000_ps_i2s_transmitter_0_0 regenerated_transmitter (
    .s_axi_ctrl_aclk(axi_clk), .s_axi_ctrl_aresetn(axi_resetn),
    .aud_mclk(bridge_mclk), .aud_mrst(1'b0),
    .s_axis_aud_aclk(axi_clk), .s_axis_aud_aresetn(axi_resetn),
    .s_axi_ctrl_awvalid(axi_awvalid), .s_axi_ctrl_awready(axi_awready),
    .s_axi_ctrl_awaddr(axi_awaddr), .s_axi_ctrl_wvalid(axi_wvalid),
    .s_axi_ctrl_wready(axi_wready), .s_axi_ctrl_wdata(axi_wdata),
    .s_axi_ctrl_bvalid(axi_bvalid), .s_axi_ctrl_bready(axi_bready),
    .s_axi_ctrl_bresp(axi_bresp), .s_axi_ctrl_arvalid(1'b0),
    .s_axi_ctrl_arready(), .s_axi_ctrl_araddr(8'b0), .s_axi_ctrl_rvalid(),
    .s_axi_ctrl_rready(1'b0), .s_axi_ctrl_rdata(), .s_axi_ctrl_rresp(),
    .irq(), .lrclk_in(regen_lrclk_delayed), .sclk_in(regenerated_sclk),
    .sdata_0_out(regenerated_sdata), .s_axis_aud_tdata(tx_data),
    .s_axis_aud_tid(tx_tid), .s_axis_aud_tvalid(tx_valid),
    .s_axis_aud_tready(tx_ready)
  );

  zz9000_ps_i2s_transmitter_0_0 direct_reference_transmitter (
    .s_axi_ctrl_aclk(axi_clk), .s_axi_ctrl_aresetn(axi_resetn),
    .aud_mclk(bridge_mclk), .aud_mrst(1'b0),
    .s_axis_aud_aclk(axi_clk), .s_axis_aud_aresetn(axi_resetn),
    .s_axi_ctrl_awvalid(axi_awvalid), .s_axi_ctrl_awready(),
    .s_axi_ctrl_awaddr(axi_awaddr), .s_axi_ctrl_wvalid(axi_wvalid),
    .s_axi_ctrl_wready(), .s_axi_ctrl_wdata(axi_wdata), .s_axi_ctrl_bvalid(),
    .s_axi_ctrl_bready(axi_bready), .s_axi_ctrl_bresp(),
    .s_axi_ctrl_arvalid(1'b0), .s_axi_ctrl_arready(), .s_axi_ctrl_araddr(8'b0),
    .s_axi_ctrl_rvalid(), .s_axi_ctrl_rready(1'b0), .s_axi_ctrl_rdata(),
    .s_axi_ctrl_rresp(), .irq(), .lrclk_in(codec_lrclk), .sclk_in(divided_bclk),
    .sdata_0_out(reference_sdata), .s_axis_aud_tdata(tx_data),
    .s_axis_aud_tid(tx_tid), .s_axis_aud_tvalid(tx_valid),
    .s_axis_aud_tready(reference_ready)
  );

  always #5 fclk_in = ~fclk_in;
  always #20 axi_clk = ~axi_clk;

  initial begin
    if (!$value$plusargs("LRCLK_DELAY_NS=%f", lrclk_delay_ns))
      lrclk_delay_ns = 35.0;
    if (!$value$plusargs("BCLK_SKEW_NS=%f", bclk_skew_ns))
      bclk_skew_ns = 0.0;
    if (lrclk_delay_ns < 0.0 || lrclk_delay_ns > 40.0)
      $fatal(1, "LRCLK_DELAY_NS must be in the ADAU1701 0..40 ns window");
    if (bclk_skew_ns < -0.5 || bclk_skew_ns > 0.5)
      $fatal(1, "BCLK_SKEW_NS must stay inside the pad-skew envelope");
    /* Only non-negative procedural delays are legal: model negative BCLK
     * skew as extra LRCLK-side insertion delay instead. */
    bclk_delay_ns_used = (bclk_skew_ns > 0.0) ? bclk_skew_ns : 0.0;
    lrclk_extra_delay_ns = (bclk_skew_ns < 0.0) ? -bclk_skew_ns : 0.0;
    disconnect_tx_sclk = $test$plusargs("DISCONNECT_TX_SCLK");
  end

  always #(BCLK_HALF_NS) if (codec_run) codec_bclk = ~codec_bclk;

  /* BCLK reaches the FPGA through its own input path; the skew sweeps the
   * pad-to-pad insertion difference between both codec outputs. */
  always @(codec_bclk) begin
    #(bclk_delay_ns_used);
    dut_bclk = codec_bclk;
  end

  /* LRCLK reaches the regenerator through IBUF plus the kept input LUTs. */
  always @(codec_lrclk) begin
    #(LRCLK_INPUT_DELAY_NS + lrclk_extra_delay_ns);
    dut_lrclk = codec_lrclk;
  end

  /* The regenerated LRCLK reaches the transmitter through the kept output
   * LUTs, past a coincident clkgen rising edge's capture instant. */
  always @(regenerated_lrclk) begin
    #(REGEN_OUTPUT_DELAY_NS);
    regen_lrclk_delayed = regenerated_lrclk;
  end

  /* tSODM: the codec changes LRCLK after its own falling BCLK edge. */
  always @(negedge codec_bclk) begin
    if (codec_run && codec_tdm) begin
      codec_count = codec_count + 1;
      if (codec_count >= codec_half) begin
        #(lrclk_delay_ns) codec_lrclk = ~codec_lrclk;
        codec_count = 0;
        if (codec_lrclk == 1'b0)
          codec_falls = codec_falls + 1;
      end
    end
  end

  task automatic codec_stop;
    begin
      codec_run = 1'b0;
    end
  endtask

  task automatic codec_start_static(input reg level);
    begin
      codec_half = 128;
      codec_count = 0;
      codec_falls = 0;
      codec_tdm = 1'b0;
      codec_lrclk = level;
      codec_run = 1'b1;
    end
  endtask

  task automatic codec_start_framed(input integer half,
                                    input integer phase_offset);
    begin
      codec_half = half;
      codec_count = phase_offset % half;
      codec_falls = 0;
      codec_tdm = 1'b1;
      codec_lrclk = 1'b1;
      codec_run = 1'b1;
    end
  endtask

  task automatic axi_write(input reg [7:0] address, input reg [31:0] data);
    reg address_done;
    reg data_done;
    begin
      @(negedge axi_clk);
      axi_awaddr = address;
      axi_awvalid = 1'b1;
      axi_wdata = data;
      axi_wvalid = 1'b1;
      axi_bready = 1'b1;
      address_done = 1'b0;
      data_done = 1'b0;
      while (!address_done || !data_done) begin
        @(posedge axi_clk);
        if (axi_awvalid && axi_awready) address_done = 1'b1;
        if (axi_wvalid && axi_wready) data_done = 1'b1;
        @(negedge axi_clk);
        if (address_done) axi_awvalid = 1'b0;
        if (data_done) axi_wvalid = 1'b0;
      end
      while (!axi_bvalid) @(posedge axi_clk);
      if (axi_bresp != 2'b00) $fatal(1, "AXI write response %02b", axi_bresp);
      @(negedge axi_clk);
      axi_bready = 1'b0;
    end
  endtask

  always @(posedge axi_clk)
    if (tx_valid && tx_ready) begin
      regen_words = regen_words + 1;
      if (tx_valid && reference_ready)
        tx_data <= {tx_data[30:0], tx_data[31]};
    end

  /*
   * Frame-by-frame comparison.  Inside a check window the comparator arms
   * itself only at a BCLK rise where the locked regenerated LRCLK and the
   * codec LRCLK agree low, i.e. exactly at frame lockstep; a stale lock on
   * a pre-disturbance phase never arms.  The SDATA comparison runs until
   * the injected glitch (after which the two transmitters may hold
   * different AXIS word offsets); frame alignment and AXIS consumption are
   * still verified for every later segment.
   */
  reg previous_regen_lrclk = 1'b1;

  always @(posedge codec_bclk) begin
    #1;
    if (!check_window) begin
      compare_armed = 1'b0;
    end else if (!compare_armed && bridge.tx_regen_locked &&
                 codec_lrclk == 1'b0 && previous_lrclk == 1'b1 &&
                 regenerated_lrclk == 1'b0 &&
                 previous_regen_lrclk == 1'b1) begin
      /* Both LRCLKs fell inside this BCLK period: frame lockstep.  A
       * stale lock on a pre-disturbance phase never satisfies this. */
      compare_armed = 1'b1;
      compared_frames = 0;
    end else if (compare_armed && bridge.tx_regen_locked &&
                 glitch_guard_frames == 0) begin
      if (regenerated_lrclk !== codec_lrclk)
        $fatal(1, "regenerated LRCLK %b differs from codec %b at %0t lockp=%0d phase=%0d",
               regenerated_lrclk, codec_lrclk, $time, bridge.tx_lock_phase,
               bridge.tx_phase);
      if (compare_sdata && regenerated_sdata !== reference_sdata)
        $fatal(1, "regenerated transmitter SDATA differs from direct reference at %0t",
               $time);
      compared_bits = compared_bits + 1;
    end
    if (codec_lrclk != previous_lrclk && !codec_lrclk) begin
      if (compare_armed)
        compared_frames = compared_frames + 1;
      if (glitch_guard_frames > 0)
        glitch_guard_frames = glitch_guard_frames - 1;
    end
    previous_lrclk = codec_lrclk;
    previous_regen_lrclk = regenerated_lrclk;
  end

  task automatic run_locked_check(input integer words_before);
    integer guard;
    begin
      guard = 0;
      while (!bridge.tx_regen_locked && guard < 40 * 256) begin
        @(negedge codec_bclk);
        guard = guard + 1;
      end
      if (!bridge.tx_regen_locked)
        $fatal(1, "regenerator failed to lock after a codec disturbance");
      compared_frames = 0;
      compare_armed = 1'b0;
      check_window = 1;
      wait (compared_frames >= REQUIRED_FRAMES);
      check_window = 1'b0;
      if (compared_bits == 0)
        $fatal(1, "no post-lock serial bits compared");
      if (regen_words <= words_before)
        $fatal(1, "regenerated transmitter did not consume AXIS audio");
    end
  endtask

  task automatic inject_low_glitch;
    begin
      check_window = 1'b0;
      wait (codec_lrclk == 1'b1 && codec_count == codec_half / 2);
      @(negedge codec_bclk);
      glitch_held_lrclk = regenerated_lrclk;
      glitch_guard_frames = 2;
      #2 codec_lrclk = 1'b0;
      repeat (2) @(negedge codec_bclk);
      #2 codec_lrclk = 1'b1;
      repeat (2) @(negedge codec_bclk);
      if (!bridge.tx_regen_locked)
        $fatal(1, "single ambiguous LRCLK sample dropped the lock");
      if (regenerated_lrclk !== glitch_held_lrclk)
        $fatal(1, "single ambiguous LRCLK sample changed the regenerated output");
    end
  endtask


  initial begin : test_sequence
    /* PS-side reset releases while the codec is still in reset and both
     * clock outputs are absent. */
    repeat (8) @(posedge fclk_in);
    bridge_resetn = 1'b1;
    repeat (8) @(posedge axi_clk);
    axi_resetn = 1'b1;
    repeat (8) @(posedge axi_clk);
    axi_write(8'h08, 32'h00000001);
    tx_valid = 1'b1;
    #60000;
    $display("[phase] codec silent done");

    /* BCLK present with LRCLK parked high through codec programming. */
    codec_start_static(1'b1);
    repeat (8 * 256) @(negedge codec_bclk);
    if (bridge.tx_regen_locked)
      $fatal(1, "regenerator locked on static LRCLK");
    $display("[phase] static LRCLK done");

    /* This is the broken block-design topology: regenerator lock continues,
     * but the transmitter's serial clock never toggles, so its AXIS sink
     * never advances—the hardware signature this regression guards. */
    if (disconnect_tx_sclk) begin
      words_mark = regen_words;
      codec_stop;
      codec_start_framed(128, 17);
      repeat (16 * 256) @(negedge codec_bclk);
      if (!bridge.tx_regen_locked)
        $fatal(1, "regenerator did not lock with TX serial clock disconnected");
      if (regen_words != words_mark)
        $fatal(1, "TX AXIS sink advanced without a serial clock");
      $display("TX serial-clock disconnect reproduction PASS words=%0d", regen_words);
      $finish;
    end

    /* Wrong serial format (64-BCLK stereo frame) before 0x081E lands. */
    codec_stop;
    codec_start_framed(32, 0);
    wait (codec_falls >= 30);
    if (bridge.tx_regen_locked)
      $fatal(1, "regenerator locked on the wrong serial format");
    $display("[phase] wrong format done falls=%0d", codec_falls);

    /* First TDM8 pass: lock, bit-exact LRCLK and SDATA, AXIS consumed. */
    codec_stop;
    codec_start_framed(128, 17);
    words_mark = regen_words;
    run_locked_check(words_mark);
    $display("[phase] first TDM8 verify done");

    /* A single ambiguous sample must not rephase or unlock. */
    inject_low_glitch;
    compare_sdata = 1'b0;
    words_mark = regen_words;
    run_locked_check(words_mark);
    $display("[phase] glitch verify done");

    /* Codec reset mid-stream: clocks freeze, then return phase-shifted
     * exactly like a second audio_adau_init pass. */
    codec_stop;
    #100000;
    codec_start_framed(128, 80);
    words_mark = regen_words;
    run_locked_check(words_mark);
    $display("[phase] codec restart verify done");

    /* Second init pass: wrong format again, then TDM8 at another phase. */
    codec_stop;
    codec_start_framed(32, 5);
    wait (codec_falls >= 30);
    if (bridge.tx_regen_locked)
      $fatal(1, "regenerator locked on the wrong serial format after restart");
    codec_stop;
    codec_start_framed(128, 100);
    words_mark = regen_words;
    run_locked_check(words_mark);

    if (bridge.tx_lrclk_toggle_count == 6'b0)
      $fatal(1, "regenerated LRCLK transition counter did not advance");

    $display("LRCLK regeneration boot-sequence PASS delay=%0f skew=%0f bits=%0d words=%0d",
             lrclk_delay_ns, bclk_skew_ns, compared_bits, regen_words);
    $finish;
  end

  initial begin
    #40000000;
    $fatal(1, "timeout");
  end
endmodule
