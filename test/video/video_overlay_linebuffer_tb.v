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
  reg [3:0] read_addr = 0;
  wire [31:0] read_data;
  wire ready;
  wire [31:0] accepted_generation;
  integer failures = 0;
  integer check_seq = 0;

  video_overlay_linebuffer #(.MAX_PIXELS(16), .ADDR_WIDTH(4)) dut(
    .axis_clk(axis_clk), .axis_resetn(resetn),
    .s_axis_tdata(tdata), .s_axis_tkeep(tkeep),
    .s_axis_tlast(tlast), .s_axis_tuser(tuser),
    .s_axis_tvalid(tvalid), .s_axis_tready(tready),
    .fetch_enable(enable), .fetch_generation(generation),
    .fetch_line(fetch_line), .fetch_request(fetch_request),
    .pixel_clk(pixel_clk), .display_enable(enable), .read_addr(read_addr),
    .displayed_line(displayed_line),
    .read_data(read_data), .displayed_line_ready(ready),
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
      while (tready !== 1'b1) @(negedge axis_clk);
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

  task request_line(input integer n);
    begin
      @(negedge pixel_clk);
      fetch_line <= n;
      fetch_request <= 1;
      @(negedge pixel_clk);
      fetch_request <= 0;
    end
  endtask

  task check_line(input integer n);
    integer i;
    integer timeout;
    reg [31:0] expected;
    begin
      check_seq = check_seq + 1;
      displayed_line = n;
      /* The bank/tag selector is combinational but has multiple levels.
       * Advance one pixel clock before sampling the ready contract. */
      @(posedge pixel_clk); #1;
      timeout = 0;
      while (!ready && timeout < 30) begin
        @(posedge pixel_clk);
        timeout = timeout + 1;
      end
      if (!ready) begin
        $display("FAIL check %0d line %0d not ready state=%0d loaded=%0d requested=%0d pending=%0d valid=%0d/%0d tag=%0d/%0d gen=%0d",
                 check_seq, n, dut.state, dut.loaded_line,
                 dut.requested_line_axis, dut.requested_line_pending,
                 dut.pixel_ready_valid0, dut.pixel_ready_valid1,
                 dut.pixel_ready_line0, dut.pixel_ready_line1,
                 accepted_generation);
        failures = failures + 1;
      end
      for (i = 0; i < 4; i = i + 1) begin
        @(negedge pixel_clk); read_addr <= i;
        @(posedge pixel_clk); @(posedge pixel_clk); #1;
        expected = 32'h10000000 | (n << 8) | i;
        if (read_data !== expected) begin
          $display("FAIL check %0d line %0d word %0d got=%h exp=%h",
                   check_seq, n, i,
                   read_data, expected);
          failures = failures + 1;
        end
      end
    end
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

    /* If the producer wraps while a later line is requested, the SOF beat
     * itself must become word zero of line zero (not land in the old bank). */
    request_line(3);
    line(0, 1); check_line(0);

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

    if (failures == 0) $display("video_overlay_linebuffer_tb: PASS");
    else $display("video_overlay_linebuffer_tb: %0d failure(s)", failures);
    $finish(failures != 0);
  end
endmodule
