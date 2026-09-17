`timescale 1ns/1ps
/* Native capture clock supervision and bounded MMCM phase requests.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
module videocap_clock_control #(
    parameter integer C28_MODE = 0,
    parameter integer WINDOW_CYCLES = 100000,
    parameter integer PHASE_TIMEOUT = 1024
) (
    input wire clk, resetn,
    input wire c28, e7m, locked, psdone,
    input wire request,
    input wire [15:0] target,
    output reg psen = 0, inc = 0,
    output wire mmcm_reset,
    output wire ready,
    output reg signed [11:0] applied = 0,
    output reg busy = 0, done = 0, error = 0,
    output reg phase_fault = 0,
    output reg [15:0] c28_count = 0, e7m_count = 0,
    output reg frequency_valid = 0,
    output wire lock_seen
);
    (* ASYNC_REG = "TRUE" *) reg [2:0] c28_sync = 0, e7m_sync = 0;
    (* ASYNC_REG = "TRUE" *) reg [2:0] lock_sync = 0;
    reg c28_prev = 0, e7m_prev = 0;
    reg [16:0] window_count = 0;
    reg [15:0] c28_edges = 0, e7m_edges = 0;
    reg [10:0] quiet = 0;
    reg [1:0] good_windows = 0;
    wire c28_edge = c28_sync[2] && !c28_prev;
    wire e7m_edge = e7m_sync[2] && !e7m_prev;
    wire count_good = c28_edges >= WINDOW_CYCLES / 4 &&
                      c28_edges <= WINDOW_CYCLES * 31 / 100;
    assign lock_seen = lock_sync[2];

    always @(posedge clk) begin
        c28_sync <= {c28_sync[1:0], c28};
        e7m_sync <= {e7m_sync[1:0], e7m};
        lock_sync <= {lock_sync[1:0], locked};
        c28_prev <= c28_sync[2];
        e7m_prev <= e7m_sync[2];
        if (!resetn) begin
            window_count <= 0;
            c28_edges <= 0;
            e7m_edges <= 0;
            c28_count <= 0;
            e7m_count <= 0;
            good_windows <= 0;
            quiet <= 0;
            frequency_valid <= 0;
        end else begin
            if (c28_edge) quiet <= 0;
            else if (!(&quiet)) quiet <= quiet + 1'b1;
            if (c28_edge && !(&c28_edges)) c28_edges <= c28_edges + 1'b1;
            if (e7m_edge && !(&e7m_edges)) e7m_edges <= e7m_edges + 1'b1;
            if (window_count == WINDOW_CYCLES - 1) begin
                window_count <= 0;
                c28_count <= c28_edges;
                e7m_count <= e7m_edges;
                c28_edges <= c28_edge ? 16'd1 : 16'd0;
                e7m_edges <= e7m_edge ? 16'd1 : 16'd0;
                if (count_good) begin
                    if (good_windows != 3) good_windows <= good_windows + 1'b1;
                    if (good_windows >= 2) frequency_valid <= 1;
                end else begin
                    good_windows <= 0;
                    frequency_valid <= 0;
                end
            end else window_count <= window_count + 1'b1;
            // Loss is recognized even if the input stops partway through a window.
            if (&quiet) begin
                frequency_valid <= 0;
                good_windows <= 0;
            end
        end
    end

    localparam WAIT_SOURCE = 0, RESET_CLOCK = 1, WAIT_LOCK = 2, RUN = 3;
    reg [1:0] clock_state = WAIT_SOURCE;
    reg [16:0] clock_wait = 0;
    reg [5:0] stable_lock = 0;
    reg restart_clock = 0;
    wire source_valid = (C28_MODE == 0) || frequency_valid;
    // Legacy E7M can supply usable clocks without asserting LOCKED.
    // Keep its real lock telemetry, but qualify capture only in C28 mode.
    wire lock_qualified = (C28_MODE == 0) || lock_seen;
    assign mmcm_reset = !resetn || !source_valid ||
                        clock_state == WAIT_SOURCE || clock_state == RESET_CLOCK;
    assign ready = clock_state == RUN && source_valid && lock_qualified && !phase_fault;

    always @(posedge clk) begin
        if (!resetn || !source_valid || restart_clock) begin
            clock_state <= WAIT_SOURCE;
            clock_wait <= 0;
            stable_lock <= 0;
        end else begin
            case (clock_state)
                WAIT_SOURCE: begin clock_state <= RESET_CLOCK; clock_wait <= 0; end
                RESET_CLOCK: begin
                    if (clock_wait == 31) begin clock_state <= WAIT_LOCK; clock_wait <= 0; end
                    else clock_wait <= clock_wait + 1'b1;
                end
                WAIT_LOCK: begin
                    if (lock_qualified) begin
                        if (stable_lock == 63) begin clock_state <= RUN; clock_wait <= 0; end
                        else stable_lock <= stable_lock + 1'b1;
                    end else stable_lock <= 0;
                    if (clock_wait == WINDOW_CYCLES - 1) begin
                        clock_state <= RESET_CLOCK;
                        clock_wait <= 0;
                        stable_lock <= 0;
                    end else clock_wait <= clock_wait + 1'b1;
                end
                RUN: if (!lock_qualified) begin clock_state <= RESET_CLOCK; clock_wait <= 0; stable_lock <= 0; end
            endcase
        end
    end

    localparam PHASE_IDLE = 0, PHASE_PULSE = 1, PHASE_WAIT = 2, PHASE_RETURN = 3;
    reg [1:0] phase_state = PHASE_IDLE;
    reg signed [11:0] goal = 0;
    reg direction = 0;
    reg [15:0] phase_wait = 0;
    wire target_valid = C28_MODE ?
        ($signed(target) >= -896 && $signed(target) <= 895) :
        ($signed(target) >= -255 && $signed(target) <= 255);

    always @(posedge clk) begin
        psen <= 0;
        restart_clock <= 0;
        if (!resetn) begin
            applied <= 0;
            goal <= 0;
            busy <= 0;
            done <= 0;
            error <= 0;
            phase_fault <= 0;
            phase_state <= PHASE_IDLE;
            phase_wait <= 0;
        end else begin
            if (mmcm_reset) begin
                // Reset establishes an observable zero phase. Retain the goal
                // so a lost clock automatically restores the selected offset.
                applied <= 0;
                busy <= 0;
                done <= 0;
                phase_state <= PHASE_IDLE;
                phase_wait <= 0;
            end else if (ready) begin
                case (phase_state)
                    PHASE_IDLE: begin
                        busy <= 0;
                        if (applied == goal) done <= 1;
                        else begin
                            direction <= goal > applied;
                            inc <= goal > applied;
                            psen <= 1;
                            busy <= 1;
                            done <= 0;
                            phase_state <= PHASE_PULSE;
                        end
                    end
                    PHASE_PULSE: begin phase_state <= PHASE_WAIT; phase_wait <= 0; end
                    PHASE_WAIT: begin
                        if (psdone) begin
                            applied <= applied + (direction ? 12'sd1 : -12'sd1);
                            phase_state <= PHASE_RETURN;
                            phase_wait <= 0;
                        end else if (phase_wait == PHASE_TIMEOUT - 1) begin
                            busy <= 0;
                            done <= 0;
                            error <= 1;
                            phase_fault <= 1;
                            restart_clock <= 1;
                            phase_state <= PHASE_IDLE;
                        end else phase_wait <= phase_wait + 1'b1;
                    end
                    // PSDONE must return low before another request is sent.
                    PHASE_RETURN: begin
                        if (!psdone) phase_state <= PHASE_IDLE;
                        else if (phase_wait == PHASE_TIMEOUT - 1) begin
                            busy <= 0;
                            done <= 0;
                            error <= 1;
                            phase_fault <= 1;
                            restart_clock <= 1;
                            phase_state <= PHASE_IDLE;
                        end else phase_wait <= phase_wait + 1'b1;
                    end
                endcase
            end else begin busy <= 0; done <= 0; end
            // New requests are retained during acquisition. A bad value leaves
            // the old goal intact; it cannot move the hardware unexpectedly.
            if (request) begin
                done <= 0;
                error <= !target_valid;
                if (target_valid) begin goal <= target[11:0]; phase_fault <= 0; end
            end
        end
    end
endmodule
