`timescale 1ns / 1ps
/*
 * MNT ZZ9000 Amiga Graphics and Coprocessor Card Firmware
 *
 * Fixed ZZ9000AX TDM8-slot-0/1 capture bridge.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

module audio_clock(
    input wire bclk_in,
    input wire fclk_in,
    input wire lrclk_in,
    input wire resetn,
    input wire sdata_in,
    output wire bclk_out,
    output wire mclk_out,
    output wire tx_lrclk_out,
    output reg rx_lrclk_out,
    output reg rx_sdata_out
    );

    /*
     * The ADAU1701 is the clock master. TDM8 and the existing playback
     * path both use its 12.288 MHz (256*Fs) output BCLK as the formatter
     * master clock. fclk_in remains in the interface for compatibility
     * with the generated block design, but the receive path is wholly
     * source-synchronous to bclk_in.
     */
    wire unused_fclk = fclk_in;
    assign mclk_out = bclk_in;

    /* resetn comes from the PS reset in the FCLK0 domain, asynchronous to
     * the source-synchronous bclk_in logic below. A two-stage synchronizer
     * asserts reset asynchronously and releases it on the falling BCLK edge
     * that the main logic uses (CDC-7). The rising-edge input samplers see
     * the release half a BCLK earlier; this does not change audio data
     * timing. */
    (* ASYNC_REG = "TRUE" *) reg [1:0] resetn_sync = 2'b00;
    always @(negedge bclk_in or negedge resetn)
      if (!resetn)
        resetn_sync <= 2'b00;
      else
        resetn_sync <= {resetn_sync[0], 1'b1};
    wire reset_active = !resetn_sync[1];

    reg [2:0] clkgen;
    assign bclk_out = clkgen[2];

    /*
     * Capture the receive stream on BCLK's rising edge.  The TDM bridge uses
     * this sample; it must not feed the playback transmitter because it is
     * one serial-bit late at the codec's maximum tSODM.
     */
    (* IOB = "TRUE" *) reg sampled_lrclk;
    (* IOB = "TRUE" *) reg sampled_sdata;

    /*
     * The playback transmitter needs a frame edge at the same BCLK edge as
     * the codec, but its direct pad-to-LUT path has only the 40 ns tSODM
     * window.  Delay the input through kept LUTs before sampling it on the
     * falling edge.  The extra stage keeps the zero-minimum tSODM edge
     * arriving after F0 even with pad-to-pad skew, while its 40 ns maximum
     * still has a full BCLK period before F1.  The sample at F1 identifies
     * the F0 boundary; the regenerated output predicts the next boundaries
     * and changes only on BCLK falling edges.
     */
    (* KEEP = "TRUE", DONT_TOUCH = "TRUE" *) wire lrclk_safe1;
    (* KEEP = "TRUE", DONT_TOUCH = "TRUE" *) wire lrclk_safe;
    (* DONT_TOUCH = "TRUE" *) LUT1 #(.INIT(2'b10)) lrclk_hold_delay (
        .I0(lrclk_in),
        .O(lrclk_safe1)
    );
    (* DONT_TOUCH = "TRUE" *) LUT1 #(.INIT(2'b10)) lrclk_hold_delay2 (
        .I0(lrclk_safe1),
        .O(lrclk_safe)
    );

    /*
     * Transmitter LRCLK regenerator.  Only a low transition preceded by a
     * nominal half-frame of continuously high LRCLK (the TDM8 frame
     * marker) steers the phase lock.  Transitions belonging to other serial
     * formats, glitches, and the first edge after a long idle are counted
     * but otherwise ignored, so a stationary disturbance can neither spoof
     * nor permanently defeat the lock.  A candidate timeout drops the lock
     * when the codec stops framing.
     */
    (* IOB = "FALSE", DONT_TOUCH = "TRUE" *) reg tx_lrclk_sampled;
    reg [7:0] tx_phase;
    reg [7:0] tx_lock_phase;
    reg [1:0] tx_lock_samples;
    reg [1:0] tx_lock_mismatches;
    reg tx_regen_locked;
    reg regenerated_tx_lrclk;
    reg [7:0] tx_high_run;
    reg [5:0] tx_lrclk_toggle_div;
    reg [5:0] tx_lrclk_toggle_count;
    reg [9:0] tx_candidate_timeout;
    localparam [1:0] TX_LOCK_CONFIRM_SAMPLES = 2'd3;
    localparam [1:0] TX_LOCK_MAX_MISMATCHES = 2'd3;
    localparam [7:0] TX_FRAME_HIGH_MIN = 8'd96;
    localparam [7:0] TX_FRAME_HIGH_MAX = 8'd160;
    localparam [9:0] TX_LOCK_TIMEOUT = 10'd512;

    wire tx_lrclk_falling = (lrclk_safe != tx_lrclk_sampled) && !lrclk_safe;
    wire [7:0] observed_tx_fall_phase = tx_phase - 8'd1;
    wire tx_frame_candidate = tx_lrclk_falling &&
        (tx_high_run >= TX_FRAME_HIGH_MIN) &&
        (tx_high_run <= TX_FRAME_HIGH_MAX);

    /*
     * The output register can change on the same BCLK falling edge that
     * clocks clkgen[2] (the transmitter's sclk) high.  Kept LUT delay
     * keeps the output transition past that edge's capture instant, so the
     * transmitter resolves the previous value exactly as it does with the
     * codec's tSODM-delayed LRCLK.
     */
    (* KEEP = "TRUE", DONT_TOUCH = "TRUE" *) wire tx_lrclk_delayed1;
    (* KEEP = "TRUE", DONT_TOUCH = "TRUE" *) wire tx_lrclk_delayed2;
    (* DONT_TOUCH = "TRUE" *) LUT1 #(.INIT(2'b10)) tx_lrclk_out_delay1 (
        .I0(regenerated_tx_lrclk),
        .O(tx_lrclk_delayed1)
    );
    (* DONT_TOUCH = "TRUE" *) LUT1 #(.INIT(2'b10)) tx_lrclk_out_delay2 (
        .I0(tx_lrclk_delayed1),
        .O(tx_lrclk_delayed2)
    );
    assign tx_lrclk_out = tx_lrclk_delayed2;

    /*
     * Count regenerated frame-clock transitions for the integration
     * simulation. This state is local to the bridge and has no block-design
     * or register-map exposure.
     */

    always @(posedge bclk_in) begin
      if (reset_active) begin
        sampled_lrclk <= 1'b1;
        sampled_sdata <= 1'b0;
      end else begin
        sampled_lrclk <= lrclk_in;
        sampled_sdata <= sdata_in;
      end
    end

    reg observed_lrclk;
    reg frame_started;
    reg midframe_seen;
    reg [8:0] raw_bit_count;
    reg [15:0] left_shift;
    reg [15:0] right_shift;
    reg [15:0] parsed_left;
    reg [15:0] parsed_right;
    reg [15:0] normalized_left;
    reg [15:0] normalized_right;

    reg serial_lrclk;
    reg [4:0] serial_bit;
    reg [15:0] serial_word;

    /*
     * LRCLK and SDATA change after a falling ADAU BCLK edge and are
     * sampled on the following rising edge. At the next falling edge,
     * raw_bit_count zero is the first bit after the serial frame delay.
     *
     * The selected production format is TDM8 with 32-bit slots. Slot 0
     * carries left in raw bits 0..15 and slot 1 carries right in raw bits
     * 32..47. A complete 256-bit frame is accepted only when its LRCLK
     * midpoint and closing edge are coherent.
     */
    always @(negedge bclk_in) begin
      if (reset_active) begin
        clkgen <= 3'b000;

        observed_lrclk <= 1'b1;
        frame_started <= 1'b0;
        midframe_seen <= 1'b0;
        raw_bit_count <= 9'b0;
        left_shift <= 16'b0;
        right_shift <= 16'b0;
        parsed_left <= 16'b0;
        parsed_right <= 16'b0;
        normalized_left <= 16'b0;
        normalized_right <= 16'b0;
        serial_lrclk <= 1'b0;
        serial_bit <= 5'd15;
        serial_word <= 16'b0;
        rx_lrclk_out <= 1'b0;
        rx_sdata_out <= 1'b0;
        tx_lrclk_sampled <= 1'b1;
        tx_phase <= 8'b0;
        tx_lock_phase <= 8'b0;
        tx_lock_samples <= 2'b0;
        tx_lock_mismatches <= 2'b0;
        tx_regen_locked <= 1'b0;
        regenerated_tx_lrclk <= 1'b1;
        tx_high_run <= 8'b0;
        tx_candidate_timeout <= 10'b0;
        tx_lrclk_toggle_div <= 6'b0;
        tx_lrclk_toggle_count <= 6'b0;
      end else begin
        /*
         * Before three coherent codec frames have established a phase, hold
         * LRCLK high rather than forwarding an asynchronous edge into the
         * transmitter.  This intentionally withholds playback at codec
         * startup/restart instead of producing a one-bit-corrupted frame.
         */
        tx_lrclk_sampled <= lrclk_safe;
        tx_phase <= tx_phase + 1'b1;
        tx_high_run <= lrclk_safe ?
            (tx_high_run != 8'hff ? tx_high_run + 8'd1 : 8'hff) : 8'd0;

        if (tx_frame_candidate)
          tx_candidate_timeout <= 10'd0;
        else if (tx_candidate_timeout != TX_LOCK_TIMEOUT)
          tx_candidate_timeout <= tx_candidate_timeout + 10'd1;


        if (tx_regen_locked && tx_candidate_timeout == TX_LOCK_TIMEOUT) begin
          tx_regen_locked <= 1'b0;
          tx_lock_samples <= 2'b0;
          tx_lock_mismatches <= 2'b0;
        end else if (tx_frame_candidate) begin
          if (tx_regen_locked) begin
            if (observed_tx_fall_phase == tx_lock_phase) begin
              tx_lock_mismatches <= 2'b0;
            end else if (tx_lock_mismatches ==
                         TX_LOCK_MAX_MISMATCHES - 1'b1) begin
              tx_regen_locked <= 1'b0;
              tx_lock_phase <= observed_tx_fall_phase;
              tx_lock_samples <= 2'd1;
              tx_lock_mismatches <= 2'b0;
            end else begin
              tx_lock_mismatches <= tx_lock_mismatches + 2'd1;
            end
          end else begin
            if (tx_lock_samples == 2'd0 ||
                observed_tx_fall_phase != tx_lock_phase) begin
              tx_lock_phase <= observed_tx_fall_phase;
              tx_lock_samples <= 2'd1;
            end else if (tx_lock_samples ==
                         TX_LOCK_CONFIRM_SAMPLES - 1'b1) begin
              tx_regen_locked <= 1'b1;
              tx_lock_mismatches <= 2'b0;
            end else begin
              tx_lock_samples <= tx_lock_samples + 2'd1;
            end
          end
        end

        if (tx_regen_locked) begin
          if (tx_phase == tx_lock_phase)
            regenerated_tx_lrclk <= 1'b0;
          else if (tx_phase == tx_lock_phase + 8'd128)
            regenerated_tx_lrclk <= 1'b1;
        end else begin
          regenerated_tx_lrclk <= 1'b1;
        end

        /*
         * Count actual regenerated frame-clock transitions after lock for
         * the integration simulation.
         */
        if (tx_regen_locked &&
            (tx_phase == tx_lock_phase ||
             tx_phase == tx_lock_phase + 8'd128)) begin
          if (tx_lrclk_toggle_div == 6'h3f) begin
            tx_lrclk_toggle_div <= 6'b0;
            tx_lrclk_toggle_count <= tx_lrclk_toggle_count + 6'd1;
          end else begin
            tx_lrclk_toggle_div <= tx_lrclk_toggle_div + 6'd1;
          end
        end


        if ((sampled_lrclk != observed_lrclk) && !sampled_lrclk) begin
          if (frame_started && raw_bit_count == 9'd255 &&
              midframe_seen) begin
            normalized_left <= parsed_left;
            normalized_right <= parsed_right;
          end else begin
            normalized_left <= 16'b0;
            normalized_right <= 16'b0;
          end

          observed_lrclk <= sampled_lrclk;
          frame_started <= 1'b1;
          midframe_seen <= 1'b0;
          raw_bit_count <= 9'b0;
          left_shift <= 16'b0;
          right_shift <= 16'b0;
          parsed_left <= 16'b0;
          parsed_right <= 16'b0;
        end else if (frame_started) begin
          raw_bit_count <= raw_bit_count + 1'b1;

          if (sampled_lrclk != observed_lrclk) begin
            observed_lrclk <= sampled_lrclk;
            if (sampled_lrclk && raw_bit_count == 9'd127)
              midframe_seen <= 1'b1;
            else
              midframe_seen <= 1'b0;
          end

          if (raw_bit_count < 9'd16) begin
            left_shift <= {left_shift[14:0], sampled_sdata};
            if (raw_bit_count == 9'd15)
              parsed_left <= {left_shift[14:0], sampled_sdata};
          end

          if (raw_bit_count >= 9'd32 && raw_bit_count < 9'd48) begin
            right_shift <= {right_shift[14:0], sampled_sdata};
            if (raw_bit_count == 9'd47)
              parsed_right <= {right_shift[14:0], sampled_sdata};
          end
        end else if (sampled_lrclk != observed_lrclk) begin
          observed_lrclk <= sampled_lrclk;
        end

        /*
         * Normalize the selected TDM slots into conventional 16-bit I2S
         * for the existing Xilinx receiver. clkgen[2] is 32*Fs. Data is
         * changed on its falling edge and sampled on the next rising edge.
         */
        if (clkgen == 3'b111) begin
          if (sampled_lrclk != serial_lrclk) begin
            rx_sdata_out <= serial_word[0];
            rx_lrclk_out <= sampled_lrclk;
            serial_lrclk <= sampled_lrclk;
            serial_word <= sampled_lrclk ?
                normalized_right : normalized_left;
            serial_bit <= 5'd15;
          end else begin
            rx_sdata_out <= serial_word[serial_bit];
            if (serial_bit != 5'd0)
              serial_bit <= serial_bit - 1'b1;
          end
        end

        clkgen <= clkgen + 1'b1;
      end
    end

endmodule
