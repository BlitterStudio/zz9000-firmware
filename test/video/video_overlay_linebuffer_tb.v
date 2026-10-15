`timescale 1ns / 1ps
module video_overlay_linebuffer_tb;
  reg axis_clk = 0, pixel_clk = 0, resetn = 0;
  always #5 axis_clk = ~axis_clk;
  always #3.4 pixel_clk = ~pixel_clk;

  reg [31:0] tdata = 0;
  reg [3:0] tkeep = 4'hf;
  reg tlast = 0, tuser = 0, tvalid = 0;
  wire tready;
  reg enable = 0;
  reg [31:0] generation = 0;
  reg [11:0] fetch_line = 0;
  reg fetch_request = 0;
  reg [11:0] displayed_line = 0;
  reg [11:0] displayed_line_next = 0;
  reg [11:0] consumed_line = 0;
  reg consumed_valid = 0;
  reg [2:0] read_addr = 0;
  wire [31:0] top_even, top_odd, bottom_even, bottom_odd;
  wire ready;
  wire fetch_ready;
  wire [31:0] accepted_generation;
  integer failures = 0;
  integer check_seq = 0;
  integer k;
  integer wait_cycles;

  video_overlay_linebuffer #(.MAX_PIXELS(16), .ADDR_WIDTH(4)) dut(
    .axis_clk(axis_clk), .axis_resetn(resetn),
    .s_axis_tdata(tdata), .s_axis_tkeep(tkeep),
    .s_axis_tlast(tlast), .s_axis_tuser(tuser),
    .s_axis_tvalid(tvalid), .s_axis_tready(tready),
    .fetch_enable(enable), .fetch_generation(generation),
    .fetch_line(fetch_line), .fetch_request(fetch_request),
    .fetch_ready(fetch_ready),
    .consumed_line(consumed_line), .consumed_valid(consumed_valid),
    .pixel_clk(pixel_clk), .display_enable(enable),
    .read_addr_even(read_addr), .read_addr_odd(read_addr),
    .displayed_line(displayed_line),
    .displayed_line_next(displayed_line_next),
    .read_top_even(top_even), .read_top_odd(top_odd),
    .read_bottom_even(bottom_even), .read_bottom_odd(bottom_odd),
    .displayed_line_ready(ready),
    .accepted_generation(accepted_generation)
  );

  initial begin
    if (dut.MEMORY_WORDS != 32) begin
      $display("FAIL line-buffer banks do not span the full address space");
      failures = failures + 1;
    end
  end

  task beat(input [31:0] value, input sof, input eol);
    begin
      @(negedge axis_clk);
      tdata <= value; tuser <= sof; tlast <= eol; tvalid <= 1;
      /* tready may depend on tuser: sample it after this beat settles. */
      #1;
      while (tready !== 1'b1) begin
        @(negedge axis_clk); #1;
      end
      @(posedge axis_clk); #1;
      @(negedge axis_clk); tvalid <= 0; tuser <= 0; tlast <= 0;
    end
  endtask

  task line(input integer n, input sof);
    begin
      beat(32'h10000000 | (n << 8) | 0, sof, 0);
      beat(32'h10000000 | (n << 8) | 1, 0, 0);
      beat(32'h10000000 | (n << 8) | 2, 0, 0);
      beat(32'h10000000 | (n << 8) | 3, 0, 1);
    end
  endtask

  /* Requests follow the fetcher's single-outstanding contract. */
  task request_line(input integer n);
    integer timeout;
    begin
      timeout = 0;
      @(negedge pixel_clk);
      while (!fetch_ready && timeout < 200) begin
        @(negedge pixel_clk);
        timeout = timeout + 1;
      end
      if (!fetch_ready) begin
        $display("FAIL request for line %0d never became ready", n);
        failures = failures + 1;
      end
      fetch_line <= n;
      fetch_request <= 1;
      @(negedge pixel_clk);
      fetch_request <= 0;
    end
  endtask

  function [31:0] word_of(input integer n, input integer i);
    word_of = 32'h10000000 | (n << 8) | i;
  endfunction

  /* Both rows of a bilinear pair must be ready and readable at once, the
   * top through one memory copy and the bottom through the other. */
  task check_pair(input integer top, input integer bottom);
    integer j;
    integer timeout;
    begin
      check_seq = check_seq + 1;
      displayed_line = top;
      displayed_line_next = bottom;
      /* The bank/tag selector is combinational but has multiple levels.
       * Advance one pixel clock before sampling the ready contract. */
      @(posedge pixel_clk); #1;
      timeout = 0;
      while (!ready && timeout < 30) begin
        @(posedge pixel_clk);
        timeout = timeout + 1;
      end
      if (!ready) begin
        $display("FAIL check %0d lines %0d/%0d not ready state=%0d loaded=%0d requested=%0d pending=%0d valid=%b gen=%0d",
                 check_seq, top, bottom, dut.state, dut.loaded_line,
                 dut.requested_line_axis, dut.requested_line_pending,
                 dut.pixel_ready_valid, accepted_generation);
        failures = failures + 1;
      end
      for (j = 0; j < 2; j = j + 1) begin
        @(negedge pixel_clk); read_addr <= j;
        @(posedge pixel_clk); @(posedge pixel_clk); #1;
        if (top_even !== word_of(top, 2 * j) ||
            top_odd !== word_of(top, 2 * j + 1) ||
            bottom_even !== word_of(bottom, 2 * j) ||
            bottom_odd !== word_of(bottom, 2 * j + 1)) begin
          $display("FAIL check %0d lines %0d/%0d words %0d/%0d got=%h %h %h %h",
                   check_seq, top, bottom, 2 * j, 2 * j + 1,
                   top_even, top_odd, bottom_even, bottom_odd);
          failures = failures + 1;
        end
      end
    end
  endtask

  task check_line(input integer n);
    check_pair(n, n);
  endtask

  initial begin
    repeat (4) @(posedge axis_clk); resetn = 1; enable = 1;

    /* Stale data before SOF must be drained, not written. */
    beat(32'hdeadbeef, 0, 1);
    line(0, 1); check_line(0);

    /* A buffer generation change must discard prefetched tags and seek a
     * fresh SOF before line zero becomes ready again. */
    generation = 1;
    while (accepted_generation != 1) @(posedge pixel_clk);
    if (ready) begin
      $display("FAIL stale line survived generation change");
      failures = failures + 1;
    end
    request_line(0);
    line(0, 1); check_line(0);

    /* Fetching the opposite bank must not change the displayed bank or its
     * ready tag. This is the margin needed for whole-row prefetch. */
    /* Skewing/changing the bundled row value without a request must not make
     * the AXI domain act on a torn intermediate value. */
    fetch_line = 3; repeat (5) @(posedge axis_clk);
    if (tready || dut.loaded_line != 0) begin
      $display("FAIL unqualified fetch-line bus change reached AXI domain");
      failures = failures + 1;
    end
    request_line(1);
    repeat (5) @(posedge pixel_clk);
    if (!ready) begin
      $display("FAIL displayed line lost readiness when fetch advanced");
      failures = failures + 1;
    end
    check_line(0);
    line(1, 0); check_line(1);
    request_line(2);
    line(2, 0); check_line(2);

    /* A firmware frame commit arrives while the completed frame is stalled
     * on its last requested row. The AXI side must acknowledge the new
     * generation before the pixel side resets its request to line zero. */
    generation = 2;
    while (accepted_generation != 2) @(posedge pixel_clk);
    request_line(0);
    line(0, 1);
    check_line(0);

    /* The VDMA restarts at vblank while a later row is still being read;
     * the firmware commits the next generation just after. The SOF beat
     * must be held, not consumed under the old generation, so the new
     * generation's first row comes from that same beat. */
    request_line(3);
    @(negedge axis_clk);
    tdata <= word_of(0, 0); tuser <= 1; tlast <= 0; tvalid <= 1;
    repeat (20) @(posedge axis_clk);
    if (tready !== 1'b0) begin
      $display("FAIL SOF accepted under the old generation while reading a later row");
      failures = failures + 1;
    end
    generation = 100;
    while (accepted_generation != 100) @(posedge pixel_clk);
    request_line(0);
    @(posedge axis_clk);
    while (tready !== 1'b1) @(posedge axis_clk);
    #1;
    @(negedge axis_clk); tvalid <= 0; tuser <= 0;
    beat(word_of(0, 1), 0, 0);
    beat(word_of(0, 2), 0, 0);
    beat(word_of(0, 3), 0, 1);
    check_line(0);

    /* Wrap requests a fresh SOF.  Drain a stale tail first. */
    request_line(3);
    /* VDMA produces rows sequentially; sparse fetching drains intervening
     * rows rather than presenting the next beat as the requested row. */
    line(1, 0);
    line(2, 0);
    line(3, 0); check_line(3);
    request_line(0);
    beat(32'hbad0bad0, 0, 1);
    line(0, 1); check_line(0);

    enable = 0; repeat (5) @(posedge pixel_clk);
    if (ready) begin
      $display("FAIL ready remained asserted while disabled");
      failures = failures + 1;
    end

    /* Firmware programs the first committed generation before enabling the
     * overlay. Enabling must still wait for the pixel-domain line-zero
     * request; otherwise VDMA can overwrite a bank before its stale ready
     * tag has been invalidated. */
    generation = 3;
    repeat (5) @(posedge axis_clk);
    enable = 1;
    while (accepted_generation != 3) @(posedge pixel_clk);
    repeat (5) @(posedge axis_clk);
    if (tready) begin
      $display("FAIL disabled generation bypassed line-zero barrier");
      failures = failures + 1;
    end
    request_line(0);
    line(0, 1); check_line(0);

    /* A vertically clipped overlay can request a nonzero first source row
     * immediately after a generation handoff. Seek from SOF and publish only
     * the requested row, without transiently presenting line zero. */
    generation = 4;
    while (accepted_generation != 4) @(posedge pixel_clk);
    request_line(2);
    line(0, 1);
    line(1, 0);
    line(2, 0); check_line(2);

    /* Lookahead: the fetcher fills every bank ahead of the display, then
     * reuses only banks holding rows the display has already passed. */
    generation = 5;
    while (accepted_generation != 5) @(posedge pixel_clk);
    request_line(0); line(0, 1);
    request_line(1); line(1, 0);
    request_line(2); line(2, 0);
    request_line(3); line(3, 0);
    repeat (40) @(posedge pixel_clk);
    if (fetch_ready) begin
      $display("FAIL fetcher offered a bank while every bank held an undisplayed row");
      failures = failures + 1;
    end
    check_line(0); check_line(3);
    check_pair(0, 1); check_pair(2, 3);
    displayed_line = 2;
    consumed_line = 2; consumed_valid = 1;
    request_line(4);
    repeat (5) @(posedge pixel_clk);
    if (fetch_ready) begin
      $display("FAIL second request offered while one was outstanding");
      failures = failures + 1;
    end
    /* A pair is ready only once both of its rows are. */
    displayed_line = 3; displayed_line_next = 4;
    repeat (3) @(posedge pixel_clk);
    if (ready) begin
      $display("FAIL pair 3/4 ready before row 4 arrived");
      failures = failures + 1;
    end
    line(4, 0);
    check_line(2); check_pair(3, 4); check_line(4);

    /* Re-requesting the row already loaded is answered from its bank. */
    request_line(4);
    request_line(5); line(5, 0);
    check_line(4); check_line(5);
    consumed_valid = 0;

    /* A row completed under the old generation can cross the clock domain
     * with, or just after, the new generation's acknowledgement: the two
     * travel through independent synchronizers. It must not mark its bank
     * ready. Inject such a completion at the CDC output in the cycle of
     * the acknowledgement (k = 0) and two cycles later (k = 1). The second
     * generation is committed while the first acknowledgement is still in
     * flight, so it must still reach the pixel side. */
    for (k = 0; k < 2; k = k + 1) begin
      generation = generation + 1;
      wait_cycles = 0;
      while (dut.accepted_generation_pixel_valid !== 1'b1 ||
             dut.accepted_generation_pixel != generation) begin
        @(posedge pixel_clk); #1;
        wait_cycles = wait_cycles + 1;
        if (wait_cycles > 200) begin
          $display("FAIL generation %0d never acknowledged", generation);
          failures = failures + 1;
          $finish(1);
        end
      end
      /* The acknowledgement is sampled at the next pixel clock edge. */
      if (k == 1) repeat (2) @(posedge pixel_clk);
      force dut.completed_line_pixel_data =
        {generation[7:0] - 8'd1, 1'b0, 2'd1, 12'd7};
      force dut.completed_line_pixel_valid = 1'b1;
      @(posedge pixel_clk); #1;
      release dut.completed_line_pixel_data;
      release dut.completed_line_pixel_valid;
      displayed_line = 7; displayed_line_next = 7;
      repeat (3) @(posedge pixel_clk); #1;
      if (ready) begin
        $display("FAIL old-generation completion marked a bank ready (case %0d)", k);
        failures = failures + 1;
      end
    end
    if (failures == 0) $display("video_overlay_linebuffer_tb: PASS");
    else $display("video_overlay_linebuffer_tb: %0d failure(s)", failures);
    $finish(failures != 0);
  end
endmodule
