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

    reg [2:0] clkgen;
    assign bclk_out = clkgen[2];

    (* IOB = "TRUE" *) reg sampled_lrclk;
    (* IOB = "TRUE" *) reg sampled_sdata;

    always @(posedge bclk_in or negedge resetn) begin
      if (!resetn) begin
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
    always @(negedge bclk_in or negedge resetn) begin
      if (!resetn) begin
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
      end else begin
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
