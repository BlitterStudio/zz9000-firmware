`timescale 1ns/1ps
`include "phase_case.vh"

// Production controller + extracted MMCM. Acknowledgement must correspond
// to a measured change in both outputs, including the full C28 phase range.
module videocap_phase_tb;
  localparam integer C28_MODE = `MODEL_C28;
  localparam real REF_PERIOD = `MODEL_PERIOD;
  localparam real CAP_PERIOD = C28_MODE ? REF_PERIOD : REF_PERIOD / 4.0;
  localparam real GRID_PERIOD = C28_MODE ? REF_PERIOD * 4.0 : REF_PERIOD;
  reg refclk = 0, e7m_physical = 0, aclk = 0, resetn = 0;
  reg reference_enabled = 1;
  reg [31:0] op = 0, value = 0;
  wire cap_clk, grid_clk;
  realtime cap_origin, grid_origin;
  integer failures = 0;
  integer reference_edges = 0;

  always begin
    #(REF_PERIOD / 2.0);
    if (reference_enabled) refclk = ~refclk;
    else refclk = 0;
  end
  always #(REF_PERIOD * 2.0) e7m_physical = ~e7m_physical;
  always #5 aclk = ~aclk;
  always @(posedge refclk) reference_edges = reference_edges + 1;

  capture_phase_dut dut(
    C28_MODE ? e7m_physical : refclk,
    C28_MODE ? refclk : 1'b0,
    aclk, resetn, op, value, cap_clk, grid_clk);

  task require;
    input condition;
    input [511:0] message;
    begin
      if (condition !== 1'b1) begin
        $display("MISMATCH: %0s at %0t", message, $time);
        failures = failures + 1;
      end
    end
  endtask

  task await_phase;
    input integer target;
    integer cycles;
    begin
      cycles = 0;
      while (!(dut.vcap_clock_ready && dut.vcap_phase_done &&
               $signed(dut.vcap_phase_applied) == target) && cycles < 100000) begin
        @(posedge aclk); #0.001;
        cycles = cycles + 1;
      end
      require(cycles < 100000, "phase completion has a bounded wait");
    end
  endtask

  task measure;
    output realtime cap_phase, grid_phase;
    realtime origin;
    begin
      // C28's grid is input/4. Use the same member of each four-edge group.
      @(negedge refclk);
      while ((reference_edges % 4) != 3) @(negedge refclk);
      @(posedge refclk);
      origin = $realtime;
      fork
        begin @(posedge cap_clk); cap_phase = $realtime - origin; end
        begin @(posedge grid_clk); grid_phase = $realtime - origin; end
      join
    end
  endtask

  function real circular_delta;
    input real after_phase, before_phase, period;
    real delta;
    begin
      delta = after_phase - before_phase;
      while (delta > period / 2.0) delta = delta - period;
      while (delta < -period / 2.0) delta = delta + period;
      circular_delta = delta;
    end
  endfunction

  task check_periods;
    realtime cap_start, grid_start, cap_elapsed, grid_elapsed;
    begin
      fork
        begin
          @(posedge cap_clk); cap_start = $realtime;
          repeat (32) @(posedge cap_clk);
          cap_elapsed = ($realtime - cap_start) / 32.0;
        end
        begin
          @(posedge grid_clk); grid_start = $realtime;
          repeat (8) @(posedge grid_clk);
          grid_elapsed = ($realtime - grid_start) / 8.0;
        end
      join
      $display("PHASE periods capture=%0.6f grid=%0.6f ns", cap_elapsed, grid_elapsed);
      require(cap_elapsed > CAP_PERIOD - 0.005 && cap_elapsed < CAP_PERIOD + 0.005,
              "capture output has the required physical frequency");
      require(grid_elapsed > GRID_PERIOD - 0.005 && grid_elapsed < GRID_PERIOD + 0.005,
              "grid output has the required physical frequency");
    end
  endtask

  task check_target;
    input integer target;
    input integer host_commit;
    realtime cap_phase, grid_phase, cap_delta, grid_delta, expected;
    real cap_error, grid_error;
    begin
      @(negedge aclk);
      if (host_commit) begin
        dut.vcap_phase_staged[15:0] = target[15:0];
        dut.vcap_phase_commit_toggle = ~dut.vcap_phase_commit_toggle;
      end else begin
        value = target;
        op = C28_MODE ? 32'h80000020 : 32'h8000001f;
      end
      @(negedge aclk); op = 0;
      await_phase(target);
      #10000;
      measure(cap_phase, grid_phase);
      cap_delta = circular_delta(cap_phase, cap_origin, CAP_PERIOD);
      grid_delta = circular_delta(grid_phase, grid_origin, GRID_PERIOD);
      expected = target * REF_PERIOD / (32.0 * 56.0);
      cap_error = circular_delta(cap_delta, expected, CAP_PERIOD);
      grid_error = circular_delta(grid_delta, expected, GRID_PERIOD);
      $display("PHASE target=%0d cap=%0.3f grid=%0.3f expected=%0.3f ns",
               target, cap_delta, grid_delta, expected);
      require(cap_error >= -0.010 && cap_error <= 0.010 &&
              grid_error >= -0.010 && grid_error <= 0.010,
              "acknowledged target moves both clocks by expected fine steps");
    end
  endtask

  task check_loss_and_reapply;
    realtime cap_before, grid_before, cap_after, grid_after;
    real cap_error, grid_error;
    integer cycles;
    begin
      check_target(64, 0);
      measure(cap_before, grid_before);
      @(negedge refclk); reference_enabled = 0;
      cycles = 0;
      while (dut.vcap_clock_ready && cycles < 10000) begin
        @(posedge aclk); #0.001;
        cycles = cycles + 1;
      end
      require(!dut.vcap_clock_ready, "missing C28 withdraws capture readiness");
      repeat (3000) @(posedge aclk);
      require(dut.E7M_RESET && !dut.vcap_clock_frequency_valid,
              "missing C28 holds MMCM in reset");
      reference_enabled = 1;
      await_phase(64);
      #10000;
      measure(cap_after, grid_after);
      // The divided grid may restart on a different input-cycle member;
      // subpixel sampling phase must still be restored on both outputs.
      cap_error = circular_delta(cap_after, cap_before, REF_PERIOD);
      grid_error = circular_delta(grid_after, grid_before, REF_PERIOD);
      require(cap_error >= -0.010 && cap_error <= 0.010 &&
              grid_error >= -0.010 && grid_error <= 0.010,
              "lock recovery reapplies selected physical sampling phase");
      check_periods;
    end
  endtask

  initial begin
    #200 resetn = 1;
    await_phase(0);
    #10000;
    check_periods;
    measure(cap_origin, grid_origin);
    check_target(64, 0);
    check_target(-64, 1);
    check_target(0, 0);
    if (C28_MODE) begin
      check_target(895, 1);
      check_target(-896, 0);
      check_target(-1, 1);
      check_target(-896, 1);
      check_target(0, 1);
      $display("CASE C28 source loss and phase reapplication");
      check_loss_and_reapply;
    end
    if (failures == 0) $display("RESULT PASS videocap phase displacement");
    else $display("RESULT FAIL videocap phase displacement: %0d mismatches", failures);
    $finish;
  end

  initial begin
    #3000000;
    $display("RESULT FAIL videocap phase displacement: timeout");
    $finish;
  end
endmodule
