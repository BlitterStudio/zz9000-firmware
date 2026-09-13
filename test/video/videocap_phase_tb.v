`timescale 1ns/1ps

// Exercise the production phase engine and MMCM together. PSDONE alone is
// insufficient: shifting feedback and outputs can acknowledge a no-op.
module videocap_phase_tb;
  reg refclk = 0;
  reg aclk = 0;
  reg resetn = 0;
  reg [31:0] op = 0;
  reg [31:0] value = 0;
  wire cap_clk, grid_clk;
  realtime cap_origin, grid_origin;
  integer failures = 0;

  // Match the declared MMCM input period, not the physical E7M period.
  // At this modeled frequency, 64 steps = 64 * 35 / (32 * 56) = 1.25 ns.
  always #17.5 refclk = ~refclk;
  always #5 aclk = ~aclk;

  capture_phase_dut dut(refclk, aclk, resetn, op, value, cap_clk, grid_clk);

  task measure;
    output realtime cap_phase, grid_phase;
    realtime origin;
    begin
      @(posedge refclk);
      origin = $realtime;
      fork
        begin @(posedge cap_clk); cap_phase = $realtime - origin; end
        begin @(posedge grid_clk); grid_phase = $realtime - origin; end
      join
    end
  endtask

  function real phase_delta;
    input real after_phase, before_phase, period;
    real delta;
    begin
      delta = after_phase - before_phase;
      if (delta > period / 2.0) delta = delta - period;
      if (delta < -period / 2.0) delta = delta + period;
      phase_delta = delta;
    end
  endfunction

  task check_target;
    input integer target;
    input integer host_commit;
    realtime cap_phase, grid_phase, cap_delta, grid_delta, expected;
    begin
      @(negedge aclk);
      if (host_commit) begin
        dut.vcap_phase_staged = target;
        dut.vcap_phase_commit_toggle = ~dut.vcap_phase_commit_toggle;
      end else begin
        value = target;
        op = 32'h8000001f;
      end
      @(negedge aclk);
      op = 0;
      // Require target acknowledgement, then let the MMCM settle before
      // checking the observable clock edges against the input reference.
      wait (dut.vcap_phase_done &&
            $signed(dut.vcap_phase_applied) == target);
      #10000;
      measure(cap_phase, grid_phase);
      cap_delta = phase_delta(cap_phase, cap_origin, 8.75);
      grid_delta = phase_delta(grid_phase, grid_origin, 35.0);
      expected = target * 35.0 / (32.0 * 56.0);
      $display("PHASE target=%0d cap=%0.3f grid=%0.3f expected=%0.3f ns",
               target, cap_delta, grid_delta, expected);
      if (cap_delta < expected - 0.010 || cap_delta > expected + 0.010 ||
          grid_delta < expected - 0.010 || grid_delta > expected + 0.010) begin
        $display("MISMATCH: acknowledged target did not move both clocks");
        failures = failures + 1;
      end
    end
  endtask

  initial begin
    #200 resetn = 1;
    wait (dut.mmcm_adv_inst.LOCKED);
    #10000;
    measure(cap_origin, grid_origin);
    check_target(64, 0);
    check_target(-64, 1);
    check_target(0, 0);
    if (failures == 0) $display("RESULT PASS videocap phase displacement");
    else $display("RESULT FAIL videocap phase displacement: %0d mismatches", failures);
    $finish;
  end

  initial begin
    #300000;
    $display("RESULT FAIL videocap phase displacement: timeout");
    $finish;
  end
endmodule
