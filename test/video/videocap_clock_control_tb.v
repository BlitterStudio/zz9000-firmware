`timescale 1ns/1ps
`include "clock_case.vh"

// Fault-injection tests complement the real-MMCM phase displacement suite.
module videocap_clock_control_tb;
    localparam integer C28_MODE = `MODEL_C28;
    reg clk = 0, c28_fast = 0, e7m = 0;
    reg resetn = 0, source_enabled = 0, source_slow = 0;
    wire c28 = source_enabled ? (source_slow ? e7m : c28_fast) : 1'b0;
    reg locked = 0, lock_enabled = C28_MODE, psdone = 0;
    reg request = 0;
    reg [15:0] target = 0;
    wire psen, inc, mmcm_reset, ready, busy, done, error, phase_fault;
    wire frequency_valid, lock_seen;
    wire signed [11:0] applied;
    wire [15:0] c28_count, e7m_count;
    integer failures = 0, checks = 0;
    integer ack_mode = 0; // 0: normal, 1: missing PSDONE, 2: PSDONE stuck high
    integer lock_delay = 0, ack_delay = 0;
    integer hardware_phase = 0, pulse_count = 0, reset_count = 0;
    reg phase_direction = 0, reset_previous = 1;

    always #5 clk = ~clk;
    always #17.62 c28_fast = ~c28_fast;
    always #70.48 e7m = ~e7m;

    videocap_clock_control #(
        .C28_MODE(C28_MODE), .WINDOW_CYCLES(400), .PHASE_TIMEOUT(16)
    ) dut (
        .clk(clk), .resetn(resetn), .c28(c28), .e7m(e7m),
        .locked(locked), .psdone(psdone), .request(request), .target(target),
        .psen(psen), .inc(inc), .mmcm_reset(mmcm_reset), .ready(ready),
        .applied(applied), .busy(busy), .done(done), .error(error),
        .phase_fault(phase_fault), .c28_count(c28_count), .e7m_count(e7m_count),
        .frequency_valid(frequency_valid), .lock_seen(lock_seen)
    );

    // Simple MMCM response model: only the controller is under test here.
    // Actual frequency and phase displacement are measured with UNISIM in
    // videocap_phase_tb, so this model cannot stand in for physical movement.
    always @(posedge clk) begin
        reset_previous <= mmcm_reset;
        if (mmcm_reset && !reset_previous) reset_count = reset_count + 1;
        if (mmcm_reset) begin
            locked <= 0;
            lock_delay <= 0;
            psdone <= 0;
            ack_delay <= 0;
            hardware_phase <= 0;
        end else begin
            if (!lock_enabled) begin
                locked <= 0;
                lock_delay <= 0;
            end else if (lock_delay == 5) locked <= 1;
            else lock_delay <= lock_delay + 1;

            if (!(ack_mode == 2 && psdone)) psdone <= 0;
            if (psen) begin
                pulse_count = pulse_count + 1;
                phase_direction <= inc;
                ack_delay <= 3;
            end else if (ack_delay > 0) begin
                ack_delay <= ack_delay - 1;
                if (ack_delay == 1 && ack_mode != 1) begin
                    psdone <= 1;
                    hardware_phase <= hardware_phase + (phase_direction ? 1 : -1);
                end
            end
        end
    end

    task require;
        input condition;
        input [511:0] message;
        begin
            checks = checks + 1;
            if (condition !== 1'b1) begin
                failures = failures + 1;
                $display("MISMATCH: %0s at %0t ready=%b applied=%0d busy=%b done=%b error=%b fault=%b",
                         message, $time, ready, applied, busy, done, error, phase_fault);
            end
        end
    endtask

    task cycles;
        input integer count;
        begin repeat (count) @(posedge clk); #0.001; end
    endtask

    task submit;
        input integer requested_target;
        begin
            @(negedge clk); target = requested_target; request = 1;
            @(negedge clk); request = 0;
        end
    endtask

    task await_target;
        input integer wanted;
        integer waited;
        begin
            waited = 0;
            while (!(ready && done && applied == wanted) && waited < 20000) begin
                cycles(1);
                waited = waited + 1;
            end
            require(waited < 20000, "clock/phase target completes within bounded wait");
            require(hardware_phase == wanted, "every applied step had a modeled hardware acknowledgement");
            require(!busy && !phase_fault, "completion clears busy and phase fault");
        end
    endtask

    task await_fault;
        integer waited;
        begin
            waited = 0;
            while (!phase_fault && waited < 100) begin
                cycles(1);
                waited = waited + 1;
            end
            require(phase_fault && error && !ready && !done && !busy,
                    "phase acknowledgement failure is bounded and withdraws ready");
            cycles(80);
            require(hardware_phase == 0 && applied == 0,
                    "phase failure resets unknown hardware phase to zero");
        end
    endtask

    integer old_pulses, old_resets, toggle;
    initial begin
        cycles(10); resetn = 1;

        if (!C28_MODE) begin
        $display("CASE legacy capture and phase work with LOCKED permanently low");
        await_target(0);
        require(!lock_seen && !frequency_valid && !mmcm_reset,
                "legacy readiness does not fake lock or C28 frequency telemetry");
        submit(64); await_target(64);
        submit(-64); await_target(-64);

        $display("CASE legacy range remains independent of C28 range");
        old_pulses = pulse_count;
        submit(256); cycles(30);
        require(error && applied == -64 && pulse_count == old_pulses,
                "legacy positive out-of-range request cannot move phase");
        submit(-256); cycles(30);
        require(error && applied == -64 && pulse_count == old_pulses,
                "legacy negative out-of-range request cannot move phase");

        $display("CASE legacy lock toggles neither reset capture nor interrupt phase");
        old_resets = reset_count;
        submit(64);
        for (toggle = 0; toggle < 8; toggle = toggle + 1) begin
            lock_enabled = !lock_enabled;
            cycles(12);
            require(lock_seen == lock_enabled, "legacy telemetry follows actual LOCKED");
            require(ready && !mmcm_reset && reset_count == old_resets,
                    "legacy lock transitions preserve capture availability");
        end
        await_target(64);
        require(!lock_seen && !error && reset_count == old_resets,
                "legacy phase completes without lock or hidden MMCM reset");

        $display("CASE legacy phase timeout and explicit retry recover without LOCKED");
        ack_mode = 1; submit(65);
        await_fault;
        ack_mode = 0; submit(-3); await_target(-3);
        require(!lock_seen && !error, "legacy phase recovery keeps real low lock telemetry");
        end else begin
        $display("CASE missing and wrong-frequency reference remain unqualified");
        cycles(1600);
        require(!frequency_valid && !ready && mmcm_reset && c28_count == 0,
                "absent C28 is held in reset");
        source_enabled = 1; source_slow = 1;
        cycles(1600);
        require(!frequency_valid && !ready && mmcm_reset,
                "7 MHz signal on C28 cannot qualify as 28 MHz");
        require(c28_count >= 27 && c28_count <= 30,
                "wrong-frequency telemetry counts actual incoming edges");

        $display("CASE request retained through three-window source qualification");
        submit(6);
        source_slow = 0;
        cycles(400);
        require(!frequency_valid && !ready,
                "one good measurement window cannot qualify C28");
        await_target(6);
        require(frequency_valid && c28_count >= 112 && c28_count <= 115 &&
                e7m_count >= 27 && e7m_count <= 30,
                "qualified C28 and E7M telemetry has expected edge counts");

        $display("CASE invalid values preserve selected target and hardware phase");
        old_pulses = pulse_count;
        submit(896); cycles(30);
        require(error && applied == 6 && hardware_phase == 6 && pulse_count == old_pulses,
                "positive out-of-range target cannot move phase");
        submit(-897); cycles(30);
        require(error && applied == 6 && hardware_phase == 6 && pulse_count == old_pulses,
                "negative out-of-range target cannot move phase");
        submit(-6); await_target(-6);
        require(!error, "valid request clears prior target error");

        $display("CASE missing PSDONE times out and explicit retry recovers");
        ack_mode = 1; submit(3);
        await_fault;
        ack_mode = 0; submit(3); await_target(3);
        require(!error, "successful retry clears phase error");

        $display("CASE PSDONE stuck high also has a bounded return wait");
        ack_mode = 2; submit(5);
        await_fault;
        ack_mode = 0; submit(5); await_target(5);

        $display("CASE lock loss resets and reapplies last selected target");
        old_resets = reset_count;
        lock_enabled = 0;
        cycles(30);
        require(!ready && !done, "lock loss withdraws capture and phase readiness");
        cycles(500);
        require(reset_count > old_resets, "lock acquisition timeout restarts MMCM");
        lock_enabled = 1;
        await_target(5);

        $display("CASE source loss resets and restores retained target");
        source_enabled = 0;
        cycles(900);
        require(!frequency_valid && !ready && mmcm_reset,
                "stopped source loses qualification and resets MMCM");
        require(!done && applied == 0, "source loss invalidates old phase completion");
        source_enabled = 1;
        await_target(5);
        end

        $display("CASE AXI reset discards old goal and pending phase transaction");
        submit(-20); cycles(3);
        resetn = 0; cycles(5); resetn = 1;
        await_target(0);
        require(!error && !phase_fault, "global reset clears phase errors");

        if (failures == 0)
            $display("RESULT PASS clock control: %0d checks", checks);
        else
            $display("RESULT FAIL clock control: %0d failures / %0d checks", failures, checks);
        $finish;
    end

    initial begin
        #3000000;
        $display("RESULT FAIL clock control: global timeout");
        $finish;
    end
endmodule
