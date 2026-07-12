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
  reg [11:0] requested = 0;
  reg [3:0] read_addr = 0;
  reg read_bank = 0;
  wire [31:0] read_data;
  wire ready;
  integer failures = 0;

  video_overlay_linebuffer #(.MAX_PIXELS(16), .ADDR_WIDTH(4)) dut(
    .axis_clk(axis_clk), .axis_resetn(resetn),
    .s_axis_tdata(tdata), .s_axis_tkeep(tkeep),
    .s_axis_tlast(tlast), .s_axis_tuser(tuser),
    .s_axis_tvalid(tvalid), .s_axis_tready(tready),
    .fetch_enable(enable), .fetch_generation(generation),
    .requested_line(requested),
    .pixel_clk(pixel_clk), .read_addr(read_addr), .read_bank(read_bank),
    .read_data(read_data), .requested_line_ready(ready)
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

  task check_line(input integer n);
    integer i;
    reg [31:0] expected;
    begin
      read_bank = n & 1;
      requested = n;
      repeat (5) @(posedge pixel_clk);
      if (!ready) begin
        $display("FAIL line %0d not ready", n);
        failures = failures + 1;
      end
      for (i = 0; i < 4; i = i + 1) begin
        @(negedge pixel_clk); read_addr <= i;
        @(posedge pixel_clk); @(posedge pixel_clk); #1;
        expected = 32'h10000000 | (n << 8) | i;
        if (read_data !== expected) begin
          $display("FAIL line %0d word %0d got=%h exp=%h", n, i,
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
    generation = 1; repeat (5) @(posedge axis_clk);
    if (ready) begin
      $display("FAIL stale line survived generation change");
      failures = failures + 1;
    end
    line(0, 1); check_line(0);

    requested = 1; repeat (4) @(posedge axis_clk);
    line(1, 0); check_line(1);
    requested = 2; repeat (4) @(posedge axis_clk);
    line(2, 0); check_line(2);

    /* If the producer wraps while a later line is requested, the SOF beat
     * itself must become word zero of line zero (not land in the old bank). */
    requested = 3; repeat (4) @(posedge axis_clk);
    line(0, 1); check_line(0);

    /* Wrap requests a fresh SOF.  Drain a stale tail first. */
    requested = 0; repeat (4) @(posedge axis_clk);
    beat(32'hbad0bad0, 0, 1);
    line(0, 1); check_line(0);

    enable = 0; repeat (5) @(posedge pixel_clk);
    if (ready) begin
      $display("FAIL ready remained asserted while disabled");
      failures = failures + 1;
    end

    if (failures == 0) $display("video_overlay_linebuffer_tb: PASS");
    else $display("video_overlay_linebuffer_tb: %0d failure(s)", failures);
    $finish(failures != 0);
  end
endmodule
