`timescale 1ns / 1ps
/*
 * MNT ZZ9000 Amiga Graphics and Coprocessor Card Firmware
 * Video Stream Formatter
 *
 * Copyright (C) 2019-2026, Lucie L. Hartmann <lucie@mntre.com>
 *                          MNT Research GmbH, Berlin
 *                          https://mntre.com
 *
 * More Info: https://mntre.com/zz9000
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * GNU General Public License v3.0 or later
 *
 * https://spdx.org/licenses/GPL-3.0-or-later.html
 *
*/

module video_formatter(
  input [63:0] m_axis_vid_tdata,
  input [7:0]  m_axis_vid_tkeep,
  input m_axis_vid_tlast,
  output m_axis_vid_tready,
  input [0:0]  m_axis_vid_tuser,
  input m_axis_vid_tvalid,
  input m_axis_vid_aclk,
  input aresetn,

  input [31:0] overlay_axis_tdata,
  input [3:0]  overlay_axis_tkeep,
  input overlay_axis_tlast,
  output overlay_axis_tready,
  input overlay_axis_tuser,
  input overlay_axis_tvalid,

  input dvi_clk,
  output reg dvi_hsync,
  output reg dvi_vsync,
  output reg dvi_active_video,
  output reg [31:0] dvi_rgb,

  // control inputs for setting palette, width/height, scaling
  input [31:0] control_data,
  input [7:0] control_op,
  input control_interlace,
  output reg [1:0]control_vblank,
  input [7:0] scanline_intensity,
  input [1:0] scanline_width,
  input        scanline_parity,
  input [7:0]  scanline_intensity2
);

localparam OP_COLORMODE=1;
localparam OP_DIMENSIONS=2;
localparam OP_PALETTE=3;
localparam OP_SCALE=4;
localparam OP_VSYNC=5;
localparam OP_MAX=6;
localparam OP_HS=7;
localparam OP_VS=8;
localparam OP_THRESH=9;
localparam OP_POLARITY=10;
localparam OP_RESET=11;
localparam OP_UNUSED1=12;
localparam OP_SPRITEXY=13;
localparam OP_SPRITE_ADDR=14;
localparam OP_SPRITE_DATA=15;
localparam OP_VIDEOCAP=16; // we ignore this here, it's snooped by MNTZorro
localparam OP_REPORT_LINE=17;
localparam OP_PALETTE_SEL=18; // switch display to secondary 256 color palette for screen split
localparam OP_PALETTE_HI=19; // set values in secondary 256 color palette for screen split
localparam OP_DPMS=21;
localparam OP_OVERLAY_CTRL=22;
localparam OP_OVERLAY_POS=23;
localparam OP_OVERLAY_SIZE=24;
localparam OP_OVERLAY_KEY=25;
localparam OP_OVERLAY_SOURCE_SIZE=26;
localparam OP_OVERLAY_FRAME=27;
localparam OP_VIEWPORT_POS=28;
localparam OP_VIEWPORT_SIZE_COMMIT=29;

localparam DPMS_ON=0;
localparam DPMS_STANDBY=1; // HSync disabled, VSync enabled
localparam DPMS_SUSPEND=2; // HSync enabled, VSync disabled
localparam DPMS_OFF=3;

localparam CMODE_8BIT=0;
localparam CMODE_16BIT=1;
localparam CMODE_32BIT=2;
localparam CMODE_15BIT=3;

reg [11:0] screen_width;
reg [11:0] screen_height;
reg scale_x = 0;
reg [1:0] scale_y = 2'd1; // amiga boots in 640x256, so double the resolution vertically
reg [23:0] palette[511:0];
reg [2:0] colormode = CMODE_32BIT;
reg vsync_request;
reg sync_polarity = 1; // negative polarity
reg selected_palette = 0;
reg [1:0] dpms_level = DPMS_ON;

/* P96 PIP pending state. Firmware writes a complete set while the plane is
 * disabled; the pixel domain snapshots it in vblank. */
reg overlay_enable = 0;
reg overlay_key_enable = 0;
reg [2:0] overlay_variant = 0;
reg [1:0] overlay_source_mode = 0;
reg signed [15:0] overlay_x = 0;
reg signed [15:0] overlay_y = 0;
reg [15:0] overlay_width = 0;
reg [15:0] overlay_height = 0;
reg [15:0] overlay_source_width = 0;
reg [15:0] overlay_source_height = 0;
reg [23:0] overlay_key_rgb = 0;
reg [31:0] overlay_frame_generation = 0;

/* Scaling ratios are reduced to exact quotient/remainder steps outside the
 * pixel path. A small restoring divider runs only after geometry changes;
 * scanout itself then needs one add, one compare, and one subtract. */
reg [15:0] overlay_calc_x = 0;
reg [15:0] overlay_calc_y = 0;
reg [15:0] overlay_calc_width = 0;
reg [15:0] overlay_calc_height = 0;
reg [15:0] overlay_calc_source_width = 0;
reg [15:0] overlay_calc_source_height = 0;
reg [31:0] overlay_calc_x_clip_product = 0;
reg [31:0] overlay_calc_y_clip_product = 0;
reg [15:0] overlay_x_step_integer = 0;
reg [15:0] overlay_x_step_remainder = 0;
reg [15:0] overlay_y_step_integer = 0;
reg [15:0] overlay_y_step_remainder = 0;
reg [15:0] overlay_x_start_source = 0;
reg [15:0] overlay_x_start_remainder = 0;
reg [15:0] overlay_y_start_source = 0;
reg [15:0] overlay_y_start_remainder = 0;
reg overlay_scale_ready = 0;
reg [31:0] overlay_scale_epoch = 0;

reg overlay_div_busy = 0;
reg [1:0] overlay_div_kind = 0;
reg [5:0] overlay_div_bit = 0;
reg [31:0] overlay_dividend = 0;
reg [15:0] overlay_divisor = 1;
reg [31:0] overlay_div_quotient = 0;
reg [16:0] overlay_div_remainder = 0;

wire [15:0] overlay_clip_x =
  overlay_x < 0 ? (~overlay_x[15:0] + 1'b1) : 16'b0;
wire [15:0] overlay_clip_y =
  overlay_y < 0 ? (~overlay_y[15:0] + 1'b1) : 16'b0;
wire overlay_geometry_changed =
  overlay_calc_x != overlay_x[15:0] ||
  overlay_calc_y != overlay_y[15:0] ||
  overlay_calc_width != overlay_width ||
  overlay_calc_height != overlay_height ||
  overlay_calc_source_width != overlay_source_width ||
  overlay_calc_source_height != overlay_source_height;
wire [16:0] overlay_div_shifted_remainder =
  {overlay_div_remainder[15:0], overlay_dividend[overlay_div_bit]};
wire overlay_div_subtract =
  overlay_div_shifted_remainder >= {1'b0, overlay_divisor};
wire [16:0] overlay_div_result_remainder =
  overlay_div_subtract
    ? overlay_div_shifted_remainder - {1'b0, overlay_divisor}
    : overlay_div_shifted_remainder;
wire [31:0] overlay_div_result_quotient =
  overlay_div_quotient |
  (overlay_div_subtract ? (32'b1 << overlay_div_bit) : 32'b0);

reg [15:0] screen_h_max;
reg [15:0] screen_v_max;
reg [15:0] screen_h_sync_start;
reg [15:0] screen_h_sync_end;
reg [15:0] screen_v_sync_start;
reg [15:0] screen_v_sync_end;

localparam MAXWIDTH=2560;              // line buffer capacity in 32-bit words
localparam LINE_BUFFER_BEATS=1280;     // 64-bit words used in each bank
localparam LINE_BUFFER_BANK_ADDR_WIDTH=11;
localparam LINE_BUFFER_READ_ADDR_WIDTH=13;
localparam LINE_BUFFER_MEMORY_BITS=(1 << LINE_BUFFER_READ_ADDR_WIDTH) * 32;

// (input) vdma state
reg [3:0] next_input_state;
reg [11:0] inptr;
reg ready_for_vdma;
reg input_line_bank = 0;

assign m_axis_vid_tready = ready_for_vdma;

reg [11:0] counter_x; // vga domain
reg [11:0] counter_y; // vga domain
reg [11:0] need_line_fetch; // vga domain
reg [11:0] need_line_fetch_candidate = 0;
reg [11:0] need_line_fetch_upper_bound = 0;
reg need_line_fetch_lower_valid = 0;
reg need_line_fetch_row_valid = 0;

reg [11:0] need_line_fetch_reg;
reg [11:0] need_line_fetch_reg2;
reg [11:0] need_line_fetch_reg3;
reg [11:0] last_line_fetch;

wire pixin_lo_valid = |m_axis_vid_tkeep[3:0];
wire pixin_hi_valid = |m_axis_vid_tkeep[7:4];
wire [1:0] pixin_word_count = {1'b0, pixin_lo_valid} + {1'b0, pixin_hi_valid};
wire pixin_valid = m_axis_vid_tvalid;
wire pixin_end_of_line = m_axis_vid_tlast;
wire pixin_framestart = m_axis_vid_tuser[0];
// One 64-bit beat carries two consecutive 32-bit framebuffer words. inptr
// counts 32-bit words and stays even mid-line (the VDMA only produces a
// partial tkeep on the tlast beat), so a beat maps 1:1 onto the write port's
// byte lanes and tkeep doubles as the byte write enable.
wire pixin_in_range = inptr[11:1] < LINE_BUFFER_BEATS;
wire [7:0] line_buffer_we = (pixin_valid && ready_for_vdma && pixin_in_range)
                            ? m_axis_vid_tkeep : 8'h00;
wire line_buffer_write_bank = pixin_framestart ? 1'b0 : input_line_bank;

reg [1:0] scale_y_effective;

reg need_frame_sync; // vga domain
reg need_frame_sync_reg; // fetch domain

// sprite
localparam SPRITE_W = 32;
localparam SPRITE_H = 48;
localparam SPRITE_SIZE = SPRITE_W*SPRITE_H;
reg [23:0] sprite_buffer[SPRITE_SIZE-1:0];
reg [11:0] sprite_addr_in;
reg [11:0] sprite_x;
reg [11:0] sprite_y;
reg sprite_dbl;
reg [11:0] report_y = 0;
reg vga_sprite_dbl; // vga_domain
reg [11:0] vga_sprite_x; // vga domain
reg [11:0] vga_sprite_y; // vga domain
reg [11:0] vga_sprite_x2; // vga domain
reg [11:0] vga_sprite_y2; // vga domain
reg [11:0] sprite_px; // vga domain
reg [11:0] sprite_py; // vga domain
reg [23:0] sprite_pix; // vga domain
reg sprite_on; // vga domain
reg [23:0] sprite_pix_d1, sprite_pix_d2, sprite_pix_d3, sprite_pix_d4;
reg sprite_on_d1, sprite_on_d2, sprite_on_d3, sprite_on_d4;
reg [11:0] vga_report_y; // vga domain
reg [11:0] vga_report_y_next; // vga domain
reg vga_selected_palette; // vga domain
reg [1:0]  vga_scanline_width;
reg        vga_scanline_parity;
reg vga_scanlines_en;
reg [31:0] pixout_sl;
reg [11:0] counter_y_d1;
reg [11:0] counter_y_d2;

always @(posedge m_axis_vid_aclk)
  begin
    if (~aresetn) begin
      ready_for_vdma <= 0;
      next_input_state <= 0;
      inptr <= 0;
      input_line_bank <= 0;
    end

    need_frame_sync_reg <= need_frame_sync;
    need_line_fetch_reg  <= need_line_fetch; // sync to clock domain
    need_line_fetch_reg2 <= need_line_fetch_reg>>scale_y_effective; // line duplication

    scale_y_effective <= scale_y;

    if (pixin_valid && ready_for_vdma) begin
      // disabling this makes the picture go wild
      if (pixin_framestart) // we might have missed the frame start
        inptr <= pixin_word_count;
      else if (pixin_end_of_line) // next after this is the first pixel of the line (0)
        inptr <= 0;
      else
        inptr <= inptr + pixin_word_count;

      if (pixin_framestart)
        input_line_bank <= pixin_end_of_line ? 1'b1 : 1'b0;
      else if (pixin_end_of_line)
        input_line_bank <= ~input_line_bank;
    end

    // one-hot encoded
    case (next_input_state)
      4'h0: begin
          // wait for start of frame
          ready_for_vdma <= 1;
          if (pixin_framestart)
            next_input_state <= 4'h4;
        end
      4'h1: begin
          // reading from vdma
          last_line_fetch <= need_line_fetch_reg2;

          if (pixin_valid && pixin_end_of_line) begin
            ready_for_vdma <= 0;
            next_input_state <= 4'h2;
          end else
            ready_for_vdma <= 1; // moved here
        end
      4'h2: begin
          // we've read more than enough of this line, wait until it's time for the next

          if (vsync_request) begin
            next_input_state <= 4'h0;
          end
          else if (need_line_fetch_reg2!=last_line_fetch) begin
            // time to read the next line
            next_input_state <= 4'h1;
            //ready_for_vdma <= 1; // from here
          end
        end
      4'h4: begin
          // we are at frame start, wait for the first line of video output
          ready_for_vdma <= 0;

          // Resume line zero during vertical blank. Moving only to the wait
          // state leaves the held SOF burst stalled until the first active
          // output row requests another line.
          if (need_frame_sync_reg==1) begin
            ready_for_vdma <= 1;
            last_line_fetch <= 0;
            next_input_state <= 4'h1;
          end
        end
    endcase
  end

reg [31:0] control_data_in = 0;
reg [7:0] control_op_in = 0;
reg control_interlace_in = 0;
reg [31:0] control_data_in2 = 0;
reg [7:0] control_op_in2 = 0;
reg control_interlace_in2 = 0;

/* Viewport position and size form one mode transaction.  The source-side
 * bundle is held unchanged for the complete XPM handshake.  A second slot
 * retains the latest complete mode while one bundle is in flight; therefore
 * OP29 can replace an implicit full-canvas request queued by OP_DIMENSIONS
 * without ever tearing the payload already crossing to the pixel clock. */
localparam [1:0] VIEWPORT_IDLE   = 2'd0;
localparam [1:0] VIEWPORT_LOAD   = 2'd1;
localparam [1:0] VIEWPORT_SEND   = 2'd2;
localparam [1:0] VIEWPORT_RETURN = 2'd3;
reg [11:0] viewport_staged_x = 0;
reg [11:0] viewport_staged_y = 0;
reg [47:0] viewport_queued_payload = 0;
reg viewport_queue_valid = 0;
reg [47:0] viewport_cdc_payload = 0;
reg viewport_cdc_send = 0;
reg [1:0] viewport_cdc_state = VIEWPORT_IDLE;
wire viewport_cdc_received;
wire [47:0] viewport_dest_payload;
wire viewport_dest_req;
reg viewport_dest_ack = 0;
wire viewport_control_event =
  control_op_in != control_op_in2 || control_data_in != control_data_in2;
wire viewport_dimensions_event =
  viewport_control_event && control_op_in == OP_DIMENSIONS;
wire viewport_commit_event =
  viewport_control_event && control_op_in == OP_VIEWPORT_SIZE_COMMIT;
wire viewport_geometry_unsettled =
  viewport_cdc_state != VIEWPORT_IDLE || viewport_queue_valid ||
  viewport_dimensions_event || viewport_commit_event;
wire viewport_unsettled_pixel;

xpm_cdc_handshake #(
  .DEST_EXT_HSK(1),
  .DEST_SYNC_FF(3),
  .INIT_SYNC_FF(1),
  .SIM_ASSERT_CHK(0),
  .SRC_SYNC_FF(3),
  .WIDTH(48)
) viewport_control_cdc (
  .src_clk(m_axis_vid_aclk),
  .src_in(viewport_cdc_payload),
  .src_send(viewport_cdc_send),
  .src_rcv(viewport_cdc_received),
  .dest_clk(dvi_clk),
  .dest_out(viewport_dest_payload),
  .dest_req(viewport_dest_req),
  .dest_ack(viewport_dest_ack)
);

xpm_cdc_single #(
  .DEST_SYNC_FF(3),
  .INIT_SYNC_FF(1),
  .SIM_ASSERT_CHK(0),
  .SRC_INPUT_REG(1)
) viewport_unsettled_cdc (
  .src_clk(m_axis_vid_aclk),
  .src_in(viewport_geometry_unsettled),
  .dest_clk(dvi_clk),
  .dest_out(viewport_unsettled_pixel)
);

always @(posedge m_axis_vid_aclk) begin
  if (!aresetn) begin
    viewport_staged_x <= 0;
    viewport_staged_y <= 0;
    viewport_queued_payload <= 0;
    viewport_queue_valid <= 0;
    viewport_cdc_payload <= 0;
    viewport_cdc_send <= 0;
    viewport_cdc_state <= VIEWPORT_IDLE;
  end else begin
    case (viewport_cdc_state)
      VIEWPORT_IDLE: begin
        if (viewport_queue_valid) begin
          viewport_cdc_payload <= viewport_queued_payload;
          viewport_queue_valid <= 0;
          viewport_cdc_state <= VIEWPORT_LOAD;
        end
      end
      VIEWPORT_LOAD: begin
        viewport_cdc_send <= 1;
        viewport_cdc_state <= VIEWPORT_SEND;
      end
      VIEWPORT_SEND: begin
        if (viewport_cdc_received) begin
          viewport_cdc_send <= 0;
          viewport_cdc_state <= VIEWPORT_RETURN;
        end
      end
      VIEWPORT_RETURN: begin
        if (!viewport_cdc_received)
          viewport_cdc_state <= VIEWPORT_IDLE;
      end
    endcase

    if (viewport_control_event && control_op_in == OP_VIEWPORT_POS) begin
      viewport_staged_y <= control_data_in[27:16];
      viewport_staged_x <= control_data_in[11:0];
    end

    if (viewport_dimensions_event) begin
      viewport_staged_x <= 0;
      viewport_staged_y <= 0;
      viewport_queued_payload <= {
        12'b0, 12'b0, control_data_in[27:16], control_data_in[11:0]
      };
      viewport_queue_valid <= 1;
    end else if (viewport_commit_event) begin
      viewport_queued_payload <= {
        viewport_staged_x, viewport_staged_y,
        control_data_in[27:16], control_data_in[11:0]
      };
      viewport_queue_valid <= 1;
    end
  end
end

// control input
always @(posedge m_axis_vid_aclk)
begin
  control_op_in        <= control_op;
  control_data_in      <= control_data;
  control_interlace_in <= control_interlace;
  control_op_in2        <= control_op_in;
  control_data_in2      <= control_data_in;
  control_interlace_in2 <= control_interlace_in;

  if (next_input_state==0) begin
    vsync_request <= 0;
  end

  /* Re-arm the MM2S line phase after the pixel domain has installed a
   * rectangle.  This is essential when a queued explicit viewport follows
   * the implicit full-canvas dimensions request before scanout starts. */
  if (viewport_cdc_received)
    vsync_request <= 1;

  if (control_interlace_in != control_interlace) begin
    vsync_request <= 1;
  end

  case (control_op_in)
    OP_PALETTE: palette[{1'b0, control_data_in[31:24]}] <= control_data_in[23:0];
    OP_PALETTE_HI: palette[{1'b1, control_data_in[31:24]}] <= control_data_in[23:0];
    OP_PALETTE_SEL: selected_palette <= control_data_in[0];
    OP_DIMENSIONS: begin
        screen_height <= control_data_in[31:16];
        screen_width  <= control_data_in[11:0];
      end
    OP_SCALE: begin
        scale_x  <= control_data_in[0];
        scale_y  <= control_data_in[2:1];
        sprite_dbl <= control_data_in[3];
      end
    OP_COLORMODE: colormode  <= control_data_in[1:0]; // FIXME
    OP_VSYNC: vsync_request <= 1; //control_data[0];
    OP_MAX: begin
        screen_v_max <= control_data_in[31:16];
        screen_h_max <= control_data_in[15:0];
      end
    OP_HS: begin
        screen_h_sync_start <= control_data_in[31:16];
        screen_h_sync_end <= control_data_in[15:0];
      end
    OP_VS: begin
        screen_v_sync_start <= control_data_in[31:16];
        screen_v_sync_end <= control_data_in[15:0];
      end
    OP_THRESH: begin
      end
    OP_POLARITY: begin
        sync_polarity <= control_data_in[0];
      end
    OP_RESET: begin
      /* currently a NOP */
      end
    OP_SPRITEXY: begin
        sprite_y <= control_data_in[31:16];
        sprite_x <= control_data_in[15:0];
      end
    OP_SPRITE_ADDR: begin
        sprite_addr_in <= control_data_in[11:0];
      end
    OP_SPRITE_DATA: begin
        sprite_buffer[sprite_addr_in] <= control_data_in[23:0];
      end
    OP_REPORT_LINE: begin
        report_y <= control_data_in[11:0];
      end
    OP_DPMS: begin
        dpms_level <= control_data_in[1:0];
      end
    OP_OVERLAY_CTRL: begin
        overlay_enable <= control_data_in[0];
        overlay_key_enable <= control_data_in[1];
        overlay_source_mode <= control_data_in[3:2];
        overlay_variant <= control_data_in[6:4];
      end
    OP_OVERLAY_POS: begin
        overlay_y <= control_data_in[31:16];
        overlay_x <= control_data_in[15:0];
      end
    OP_OVERLAY_SIZE: begin
        overlay_height <= control_data_in[31:16];
        overlay_width <= control_data_in[15:0];
      end
    OP_OVERLAY_KEY: overlay_key_rgb <= control_data_in[23:0];
    OP_OVERLAY_SOURCE_SIZE: begin
        overlay_source_height <= control_data_in[31:16];
        overlay_source_width <= control_data_in[15:0];
      end
    OP_OVERLAY_FRAME: overlay_frame_generation <= control_data_in;
  endcase
end

/* Restart from the latest complete geometry whenever any member changes.
 * Firmware writes POS/SIZE/SOURCE_SIZE while the plane is hidden; the same
 * restart behavior also makes direct formatter simulations deterministic
 * when only SIZE changes between scale cases. */
always @(posedge m_axis_vid_aclk) begin
  if (!aresetn) begin
    overlay_calc_x <= 0;
    overlay_calc_y <= 0;
    overlay_calc_width <= 0;
    overlay_calc_height <= 0;
    overlay_calc_source_width <= 0;
    overlay_calc_source_height <= 0;
    overlay_calc_x_clip_product <= 0;
    overlay_calc_y_clip_product <= 0;
    overlay_x_step_integer <= 0;
    overlay_x_step_remainder <= 0;
    overlay_y_step_integer <= 0;
    overlay_y_step_remainder <= 0;
    overlay_x_start_source <= 0;
    overlay_x_start_remainder <= 0;
    overlay_y_start_source <= 0;
    overlay_y_start_remainder <= 0;
    overlay_scale_ready <= 0;
    overlay_scale_epoch <= 0;
    overlay_div_busy <= 0;
    overlay_div_kind <= 0;
    overlay_div_bit <= 0;
    overlay_dividend <= 0;
    overlay_divisor <= 1;
    overlay_div_quotient <= 0;
    overlay_div_remainder <= 0;
  end else if (overlay_geometry_changed) begin
    overlay_calc_x <= overlay_x[15:0];
    overlay_calc_y <= overlay_y[15:0];
    overlay_calc_width <= overlay_width;
    overlay_calc_height <= overlay_height;
    overlay_calc_source_width <= overlay_source_width;
    overlay_calc_source_height <= overlay_source_height;
    overlay_calc_x_clip_product <= overlay_clip_x *
                                   overlay_source_width;
    overlay_calc_y_clip_product <= overlay_clip_y *
                                   overlay_source_height;
    overlay_scale_ready <= 0;
    overlay_div_busy <= overlay_width != 0 &&
                        overlay_height != 0 &&
                        overlay_source_width != 0 &&
                        overlay_source_height != 0;
    overlay_div_kind <= 0;
    overlay_div_bit <= 31;
    overlay_dividend <= {16'b0, overlay_source_width};
    overlay_divisor <= overlay_width != 0 ? overlay_width : 16'b1;
    overlay_div_quotient <= 0;
    overlay_div_remainder <= 0;
  end else if (overlay_div_busy) begin
    overlay_div_remainder <= overlay_div_result_remainder;
    overlay_div_quotient <= overlay_div_result_quotient;
    if (overlay_div_bit != 0) begin
      overlay_div_bit <= overlay_div_bit - 1'b1;
    end else begin
      overlay_div_bit <= 31;
      overlay_div_quotient <= 0;
      overlay_div_remainder <= 0;
      case (overlay_div_kind)
        2'd0: begin
          overlay_x_step_integer <= overlay_div_result_quotient[15:0];
          overlay_x_step_remainder <= overlay_div_result_remainder[15:0];
          overlay_div_kind <= 1;
          overlay_dividend <= {16'b0, overlay_calc_source_height};
          overlay_divisor <= overlay_calc_height;
        end
        2'd1: begin
          overlay_y_step_integer <= overlay_div_result_quotient[15:0];
          overlay_y_step_remainder <= overlay_div_result_remainder[15:0];
          overlay_div_kind <= 2;
          overlay_dividend <= overlay_calc_x_clip_product;
          overlay_divisor <= overlay_calc_width;
        end
        2'd2: begin
          overlay_x_start_source <= overlay_div_result_quotient[15:0];
          overlay_x_start_remainder <=
            overlay_div_result_remainder[15:0];
          overlay_div_kind <= 3;
          overlay_dividend <= overlay_calc_y_clip_product;
          overlay_divisor <= overlay_calc_height;
        end
        default: begin
          overlay_y_start_source <= overlay_div_result_quotient[15:0];
          overlay_y_start_remainder <=
            overlay_div_result_remainder[15:0];
          overlay_div_busy <= 0;
          overlay_scale_ready <= 1;
          overlay_scale_epoch <= overlay_scale_epoch + 1'b1;
        end
      endcase
    end
  end
end

localparam PIPE_DELAY = 4;
localparam OVERLAY_PIPE_DELAY = 4;

reg [31:0] palout;
reg [11:0] vga_v_rez;
reg [11:0] vga_h_rez;
reg [11:0] vga_h_rez_delayed;
reg [11:0] vga_v_max;
reg [11:0] vga_h_max;
reg [11:0] vga_h_sync_start;
reg [11:0] vga_h_sync_end;
reg [11:0] vga_h_sync_start_delayed;
reg [11:0] vga_h_sync_end_delayed;
reg [11:0] vga_v_sync_start;
reg [11:0] vga_v_sync_end;
reg [11:0] counter_scanout;
reg [2:0] vga_colormode;
reg [11:0] vga_viewport_x = 0;
reg [11:0] vga_viewport_y = 0;
reg [11:0] vga_viewport_width = 0;
reg [11:0] vga_viewport_height = 0;
reg viewport_initialized = 0;
reg viewport_geometry_ready = 0;

reg vga_scale_x = 0;
reg [1:0] vga_scale_y = 2'd0;
wire [11:0] vga_scale_y_factor = 12'd1 << vga_scale_y;
reg [31:0] pixout;
reg [7:0]  pixout8;
reg [15:0] pixout16;
wire [31:0] pixout32;
reg [31:0] pixout32_dly;
reg [31:0] pixout32_dly2;
wire [7:0] red16   = {pixout16[4:0],   pixout16[4:2]};
wire [7:0] green16 = {pixout16[10:5],  pixout16[10:9]};
wire [7:0] blue16  = {pixout16[15:11], pixout16[15:13]};
wire [7:0] red15   = {pixout16[4:0],   pixout16[4:2]};
wire [7:0] green15 = {pixout16[9:5],   pixout16[9:7]};
wire [7:0] blue15  = {pixout16[14:10], pixout16[14:12]};

reg [3:0] counter_scanout_step;
reg [3:0] counter_subpixel = 0;

reg vga_sync_polarity = 0;
reg [1:0] vga_dpms_level = DPMS_ON;

reg vga_overlay_enable = 0;
reg vga_overlay_key_enable = 0;
reg [2:0] vga_overlay_variant = 0;
reg [1:0] vga_overlay_source_mode = 0;
reg signed [15:0] vga_overlay_x = 0;
reg signed [15:0] vga_overlay_y = 0;
reg [15:0] vga_overlay_width = 0;
reg [15:0] vga_overlay_height = 0;
reg [15:0] vga_overlay_source_width = 0;
reg [15:0] vga_overlay_source_height = 0;
reg [15:0] vga_overlay_x_step_integer = 0;
reg [15:0] vga_overlay_x_step_remainder = 0;
reg [15:0] vga_overlay_y_step_integer = 0;
reg [15:0] vga_overlay_y_step_remainder = 0;
reg [15:0] vga_overlay_x_start_source = 0;
reg [15:0] vga_overlay_x_start_remainder = 0;
reg [15:0] vga_overlay_y_start_source = 0;
reg [15:0] vga_overlay_y_start_remainder = 0;
reg [31:0] vga_overlay_scale_epoch = 0;
reg [31:0] overlay_applied_scale_epoch = 0;
reg [23:0] vga_overlay_key_rgb = 0;
reg [31:0] vga_overlay_frame_generation = 0;
reg [11:0] overlay_fetch_line = 0;
reg overlay_fetch_request = 0;
reg [15:0] overlay_scale_read_x = 0;
reg [15:0] overlay_scale_read_x_d1 = 0;
reg [15:0] overlay_scale_x_error = 0;
reg [15:0] overlay_scale_source_y = 0;
reg [15:0] overlay_scale_y_error = 0;
reg [15:0] vga_overlay_x_step_threshold = 1;
reg [15:0] vga_overlay_y_step_threshold = 1;

/* Coordinate state advances with the raster counters, but is registered
 * separately so overlay window/tag arithmetic never sits on the same
 * 150 MHz path as the fetch scheduler or BRAM bank selection. */
reg signed [16:0] overlay_screen_x_position = 0;
reg signed [16:0] overlay_local_x_position = 0;
reg signed [16:0] overlay_read_x_position = 1;
reg signed [16:0] overlay_screen_x_origin = 0;
reg signed [16:0] overlay_local_x_origin = 0;
reg signed [16:0] overlay_screen_y_origin = 0;
reg signed [16:0] overlay_local_y_origin = 0;
reg signed [16:0] overlay_screen_y_row = 0;
reg signed [16:0] overlay_local_y_row = 0;
reg [11:0] overlay_displayed_line_row = 0;
reg signed [16:0] overlay_visible_x1_unclipped_pre = 0;
reg signed [16:0] overlay_visible_x0_minus_one = -1;
reg signed [16:0] overlay_visible_x1_minus_one = -1;
reg signed [16:0] viewport_output_x_position = 0;
reg [11:0] viewport_output_y_row = 0;
reg [11:0] scanout_source_line_row = 0;
reg [12:0] viewport_output_x_end = 0;
reg overlay_scheduler_scaling = 0;
reg overlay_scheduler_y_in_window = 0;
reg overlay_scheduler_screen_y_nonnegative = 0;
reg [11:0] overlay_scheduler_next_source_y = 0;

wire signed [16:0] overlay_screen_x = overlay_screen_x_position;
wire signed [16:0] overlay_screen_y = overlay_screen_y_row;
wire signed [16:0] overlay_local_x = overlay_local_x_position;
wire signed [16:0] overlay_local_y = overlay_local_y_row;
reg vga_overlay_scaling = 0;
wire overlay_in_window = vga_overlay_enable &&
  overlay_local_x >= 0 && overlay_local_y >= 0 &&
  overlay_local_x < $signed({1'b0, vga_overlay_width}) &&
  overlay_local_y < $signed({1'b0, vga_overlay_height});
wire [11:0] overlay_displayed_line = overlay_displayed_line_row;
wire signed [16:0] overlay_read_x = overlay_read_x_position;
wire [15:0] overlay_selected_read_x =
  vga_overlay_scaling ? overlay_scale_read_x : overlay_read_x[15:0];
wire overlay_selected_luma_phase =
  vga_overlay_scaling ? overlay_scale_read_x_d1[0] : overlay_local_x[0];
wire [10:0] overlay_read_addr = overlay_selected_read_x[11:1];
wire overlay_scale_x_carry =
  overlay_scale_x_error >= vga_overlay_x_step_threshold;
wire overlay_scale_y_carry =
  overlay_scale_y_error >= vga_overlay_y_step_threshold;
wire [15:0] overlay_scale_x_error_added =
  overlay_scale_x_error + vga_overlay_x_step_remainder;
wire [15:0] overlay_scale_x_error_wrapped =
  overlay_scale_x_error - vga_overlay_x_step_threshold;
wire [15:0] overlay_scale_y_error_added =
  overlay_scale_y_error + vga_overlay_y_step_remainder;
wire [15:0] overlay_scale_y_error_wrapped =
  overlay_scale_y_error - vga_overlay_y_step_threshold;
wire [16:0] overlay_scale_next_source_y =
  {1'b0, overlay_scale_source_y} +
  {1'b0, vga_overlay_y_step_integer} +
  overlay_scale_y_carry;
wire [31:0] overlay_yuv422;
wire overlay_line_ready;
wire [31:0] overlay_accepted_generation;
wire [11:0] overlay_scheduler_line;
wire overlay_scheduler_line_ready;

wire [11:0] next_raster_y = counter_y >= vga_v_max
  ? 12'b0 : counter_y + 1'b1;
wire [11:0] next_scanout_content_y = next_raster_y - vga_viewport_y;
wire [11:0] next_scanout_source_line =
  (next_raster_y >= vga_viewport_y + vga_scale_y_factor)
    ? ((next_scanout_content_y - vga_scale_y_factor) >> vga_scale_y)
    : 12'b0;

video_overlay_linebuffer overlay_linebuffer (
  .axis_clk(m_axis_vid_aclk),
  .axis_resetn(aresetn),
  .s_axis_tdata(overlay_axis_tdata),
  .s_axis_tkeep(overlay_axis_tkeep),
  .s_axis_tlast(overlay_axis_tlast),
  .s_axis_tuser(overlay_axis_tuser),
  .s_axis_tvalid(overlay_axis_tvalid),
  .s_axis_tready(overlay_axis_tready),
  .fetch_enable(overlay_enable && overlay_source_mode == 0),
  .fetch_generation(overlay_frame_generation),
  .fetch_line(overlay_fetch_line),
  .fetch_request(overlay_fetch_request),
  .pixel_clk(dvi_clk),
  .display_enable(vga_overlay_enable && vga_overlay_source_mode == 0),
  .read_addr(overlay_read_addr),
  .displayed_line(overlay_displayed_line),
  .read_data(overlay_yuv422),
  .displayed_line_ready(overlay_line_ready),
  .scheduler_line(overlay_scheduler_line),
  .scheduler_line_ready(overlay_scheduler_line_ready),
  .accepted_generation(overlay_accepted_generation)
);

wire [23:0] overlay_rgb;
wire overlay_pixel_active;
video_overlay_pixel overlay_pixel (
  .clk(dvi_clk),
  .resetn(aresetn),
  .base_rgb(pixout[23:0]),
  .key_rgb(vga_overlay_key_rgb),
  .key_enable(vga_overlay_key_enable),
  .overlay_enable(overlay_in_window && overlay_line_ready &&
                  vga_overlay_source_mode == 0),
  .yuv422(overlay_yuv422),
  .variant(vga_overlay_variant),
  .luma_phase(overlay_selected_luma_phase),
  .out_rgb(overlay_rgb),
  .out_overlay(overlay_pixel_active)
);

wire [31:0] pixout_composited = vga_overlay_enable
  ? {8'b0, overlay_rgb} : pixout;
wire [31:0] composed_rgb = vga_overlay_enable
  ? ((sprite_on_d4 && sprite_pix_d4 != 'hff00ff)
      ? sprite_pix_d4 : pixout_sl)
  : ((sprite_on && sprite_pix != 'hff00ff) ? sprite_pix : pixout_sl);
wire signed [16:0] viewport_output_x = viewport_output_x_position;
wire [11:0] viewport_output_y = viewport_output_y_row;
wire viewport_output_active = viewport_geometry_ready &&
  counter_y >= vga_scale_y_factor &&
  viewport_output_x >= $signed({1'b0, vga_viewport_x}) &&
  viewport_output_x < $signed({1'b0, viewport_output_x_end}) &&
  viewport_output_y >= vga_viewport_y &&
  viewport_output_y < vga_viewport_y + vga_viewport_height;
wire [11:0] scanline_content_y = counter_y_d2 - vga_viewport_y;

wire [11:0] scanout_source_line = scanout_source_line_row;

// Ping-pong line buffer: VDMA fills the next source line in one bank while
// scanout reads the current source line from the other. A full power-of-two
// address space is allocated because bank selection is the address MSB.
// The write side receives 64-bit VDMA beats and the read side emits 32-bit
// scanout words.
// READ_LATENCY_B(1) gives doutb the exact registered-read timing the
// original 32-bit design's inferred line_buffer read had, so every
// downstream pipeline phase (counter_subpixel unpacking, PIPE_DELAY)
// is unchanged. pixout32 must stay a direct wire from doutb: adding a
// register here shifts the sub-word phase and swaps/duplicates pixel
// columns in the 8/16/15 bpp modes.
xpm_memory_sdpram #(
  .MEMORY_SIZE(LINE_BUFFER_MEMORY_BITS),
  .MEMORY_PRIMITIVE("block"),
  .CLOCKING_MODE("independent_clock"),
  .ECC_MODE("no_ecc"),
  .MEMORY_INIT_FILE("none"),
  .MEMORY_INIT_PARAM("0"),
  .USE_MEM_INIT(0),
  .WAKEUP_TIME("disable_sleep"),
  .AUTO_SLEEP_TIME(0),
  .MESSAGE_CONTROL(0),
  .USE_EMBEDDED_CONSTRAINT(0),
  .MEMORY_OPTIMIZATION("true"),
  .WRITE_DATA_WIDTH_A(64),
  .BYTE_WRITE_WIDTH_A(8),
  .ADDR_WIDTH_A(LINE_BUFFER_BANK_ADDR_WIDTH + 1),
  .RST_MODE_A("SYNC"),
  .READ_DATA_WIDTH_B(32),
  .ADDR_WIDTH_B(LINE_BUFFER_READ_ADDR_WIDTH),
  .READ_RESET_VALUE_B("0"),
  .READ_LATENCY_B(1),
  .WRITE_MODE_B("read_first"),
  .RST_MODE_B("SYNC")
) line_buffer (
  .sleep(1'b0),
  .clka(m_axis_vid_aclk),
  .ena(1'b1),
  .wea(line_buffer_we),
  .addra({line_buffer_write_bank, inptr[11:1]}),
  .dina(m_axis_vid_tdata),
  .injectsbiterra(1'b0),
  .injectdbiterra(1'b0),
  .clkb(dvi_clk),
  .rstb(1'b0),
  .enb(1'b1),
  .regceb(1'b1),
  .addrb({scanout_source_line[0], counter_scanout}),
  .doutb(pixout32),
  .sbiterrb(),
  .dbiterrb()
);

wire viewport_frame_boundary = counter_x == 0 && counter_y == 0;

/* The destination owns the visible rectangle and acknowledges only after a
 * complete bundle has been installed at a frame boundary. */
always @(posedge dvi_clk) begin
  if (!aresetn) begin
    viewport_dest_ack <= 0;
    vga_viewport_x <= 0;
    vga_viewport_y <= 0;
    vga_viewport_width <= 0;
    vga_viewport_height <= 0;
    viewport_initialized <= 0;
  end else begin
    /* Before the first request reaches a newly started pixel clock, preserve
     * the legacy full-canvas counter/fetch phase. */
    if (!viewport_initialized) begin
      vga_viewport_x <= 0;
      vga_viewport_y <= 0;
      vga_viewport_width <= screen_width;
      vga_viewport_height <= screen_height;
    end
    if (!viewport_dest_req) begin
      viewport_dest_ack <= 0;
    end else if (!viewport_dest_ack && viewport_frame_boundary) begin
      vga_viewport_x <= viewport_dest_payload[47:36];
      vga_viewport_y <= viewport_dest_payload[35:24];
      vga_viewport_height <= viewport_dest_payload[23:12];
      vga_viewport_width <= viewport_dest_payload[11:0];
      viewport_initialized <= 1;
      viewport_dest_ack <= 1;
    end
  end
end

always @(posedge dvi_clk) begin
  overlay_fetch_request <= 0;

  if (!aresetn)
    viewport_geometry_ready <= 0;
  else if (viewport_frame_boundary) begin
    if (viewport_unsettled_pixel || viewport_dest_req)
      viewport_geometry_ready <= 0;
    else if (viewport_initialized)
      viewport_geometry_ready <= 1;
  end

  /* Preserve the established canvas timing path.  The rectangle itself is
   * atomic; geometry is blacked out while a mode transaction is unsettled. */
  vga_h_rez <= screen_width;
  vga_v_rez <= screen_height;
  vga_h_max <= screen_h_max - 1'b1;
  vga_v_max <= screen_v_max - 1'b1;
  vga_h_sync_start <= screen_h_sync_start;
  vga_h_sync_end <= screen_h_sync_end;

  vga_h_sync_start_delayed <= vga_h_sync_start + PIPE_DELAY +
                              (vga_overlay_enable ? OVERLAY_PIPE_DELAY : 0);
  vga_h_sync_end_delayed <= vga_h_sync_end + PIPE_DELAY +
                            (vga_overlay_enable ? OVERLAY_PIPE_DELAY : 0);
  vga_h_rez_delayed <= vga_h_rez + PIPE_DELAY +
                       (vga_overlay_enable ? OVERLAY_PIPE_DELAY : 0);

  vga_v_sync_start <= screen_v_sync_start;
  vga_v_sync_end <= screen_v_sync_end;
  vga_scale_x <= scale_x;
  vga_scale_y <= scale_y;
  vga_colormode <= colormode;
  vga_sync_polarity <= sync_polarity;
  vga_dpms_level <= dpms_level;
  if (counter_y == vga_v_sync_start && counter_x == 0) begin
    vga_overlay_enable <= overlay_enable && overlay_scale_ready;
    vga_overlay_key_enable <= overlay_key_enable;
    vga_overlay_variant <= overlay_variant;
    vga_overlay_source_mode <= overlay_source_mode;
    vga_overlay_x <= overlay_x;
    vga_overlay_y <= overlay_y;
    vga_overlay_width <= overlay_width;
    vga_overlay_height <= overlay_height;
    vga_overlay_source_width <= overlay_source_width;
    vga_overlay_source_height <= overlay_source_height;
    vga_overlay_x_step_integer <= overlay_x_step_integer;
    vga_overlay_x_step_remainder <= overlay_x_step_remainder;
    vga_overlay_y_step_integer <= overlay_y_step_integer;
    vga_overlay_y_step_remainder <= overlay_y_step_remainder;
    vga_overlay_x_start_source <= overlay_x_start_source;
    vga_overlay_x_start_remainder <= overlay_x_start_remainder;
    vga_overlay_y_start_source <= overlay_y_start_source;
    vga_overlay_y_start_remainder <= overlay_y_start_remainder;
    vga_overlay_scale_epoch <= overlay_scale_epoch;
    vga_overlay_key_rgb <= overlay_key_rgb;
    vga_overlay_scaling <=
      overlay_source_width != overlay_width ||
      overlay_source_height != overlay_height ||
      overlay_x < 0 || overlay_y < 0;
    vga_overlay_x_step_threshold <=
      overlay_width - overlay_x_step_remainder;
    vga_overlay_y_step_threshold <=
      overlay_height - overlay_y_step_remainder;
  end

  /* Geometry is stable for a frame. Split clipped-bound and origin
   * arithmetic across registers so per-pixel comparisons start at a
   * registered coordinate rather than at vga_overlay_x/viewport_x. */
  overlay_screen_x_origin <= -$signed(PIPE_DELAY) + 17'sd1 -
    $signed({1'b0, vga_viewport_x});
  overlay_local_x_origin <=
    overlay_screen_x_origin - $signed(vga_overlay_x);
  overlay_screen_y_origin <= -17'sd1 -
    $signed({1'b0, vga_viewport_y});
  overlay_local_y_origin <=
    overlay_screen_y_origin - $signed(vga_overlay_y);
  overlay_visible_x1_unclipped_pre <=
    $signed(vga_overlay_x) + $signed({1'b0, vga_overlay_width});
  overlay_visible_x0_minus_one <=
    (vga_overlay_x < 0 ? 17'sd0 : $signed(vga_overlay_x)) - 17'sd1;
  overlay_visible_x1_minus_one <=
    (overlay_visible_x1_unclipped_pre >
       $signed({1'b0, vga_viewport_width})
      ? $signed({1'b0, vga_viewport_width})
      : overlay_visible_x1_unclipped_pre) - 17'sd1;
  viewport_output_x_end <= {1'b0, vga_viewport_x} +
                           {1'b0, vga_viewport_width};
  overlay_scheduler_scaling <= vga_overlay_scaling;
  overlay_scheduler_y_in_window <=
    overlay_local_y >= 0 &&
    overlay_local_y < $signed({1'b0, vga_overlay_height});
  overlay_scheduler_screen_y_nonnegative <= overlay_screen_y >= 0;
  overlay_scheduler_next_source_y <= overlay_scale_next_source_y[11:0];
  overlay_scale_read_x_d1 <= overlay_scale_read_x;
  if (counter_x == 0) begin
    overlay_scale_read_x <= vga_overlay_x_start_source;
    overlay_scale_read_x_d1 <= vga_overlay_x_start_source;
    overlay_scale_x_error <= vga_overlay_x_start_remainder;
  end else if (vga_overlay_scaling &&
               overlay_screen_x >= overlay_visible_x0_minus_one &&
               overlay_screen_x < overlay_visible_x1_minus_one) begin
    overlay_scale_read_x <= overlay_scale_read_x +
      vga_overlay_x_step_integer +
      overlay_scale_x_carry;
    if (overlay_scale_x_carry)
      overlay_scale_x_error <= overlay_scale_x_error_wrapped;
    else
      overlay_scale_x_error <= overlay_scale_x_error_added;
  end

  if (counter_x == 0 && counter_y == 0) begin
    overlay_scale_source_y <= vga_overlay_y_start_source;
    overlay_scale_y_error <= vga_overlay_y_start_remainder;
  end else if (counter_x == vga_h_rez &&
               vga_overlay_scaling &&
               overlay_screen_y >= 0 &&
               overlay_local_y >= 0 &&
               overlay_local_y + 17'sd1 <
                 $signed({1'b0, vga_overlay_height})) begin
    overlay_scale_source_y <= overlay_scale_next_source_y[15:0];
    if (overlay_scale_y_carry)
      overlay_scale_y_error <= overlay_scale_y_error_wrapped;
    else
      overlay_scale_y_error <= overlay_scale_y_error_added;
  end
  if (counter_y == 0) begin
    vga_sprite_x <= sprite_x;
    vga_sprite_y <= sprite_y;
  end
  vga_sprite_x2 <= vga_sprite_x+(SPRITE_W<<sprite_dbl);
  vga_sprite_y2 <= vga_sprite_y+(SPRITE_H<<sprite_dbl);
  vga_sprite_dbl <= sprite_dbl;
  vga_report_y_next <= report_y;
  vga_selected_palette <= selected_palette;
  vga_scanline_width      <= scanline_width;
  vga_scanline_parity     <= scanline_parity;
  vga_scanlines_en <= !control_interlace &&
                    ((|scale_y) || (vga_v_rez < 350));

  /*
    pipelines (4 clocks):

    linebuf   pixout32    pixout32_dly  pixout32_dly2 pixout
    linebuf   pixout32    pixout16      pixout32_dly  pixout
    linebuf   pixout32    pixout8       palout        pixout
  */

  case ({vga_scale_x,counter_subpixel[2:0]})
    4'b0011: pixout8 <= pixout32[31:24];
    4'b0000: pixout8 <= pixout32[23:16];
    4'b0001: pixout8 <= pixout32[15:8];
    4'b0010: pixout8 <= pixout32[7:0];

    4'b1111: pixout8 <= pixout32[31:24];
    4'b1000: pixout8 <= pixout32[31:24];
    4'b1001: pixout8 <= pixout32[23:16];
    4'b1010: pixout8 <= pixout32[23:16];
    4'b1011: pixout8 <= pixout32[15:8];
    4'b1100: pixout8 <= pixout32[15:8];
    4'b1101: pixout8 <= pixout32[7:0];
    4'b1110: pixout8 <= pixout32[7:0];
  endcase

  case ({vga_scale_x,counter_subpixel[1:0]})
    3'b001: pixout16 <= {pixout32[23:16],pixout32[31:24]};
    3'b000: pixout16 <= {pixout32[7:0]  ,pixout32[15:8] };

    3'b100: pixout16 <= {pixout32[23:16],pixout32[31:24]};
    3'b111: pixout16 <= {pixout32[23:16],pixout32[31:24]};
    3'b110: pixout16 <= {pixout32[7:0]  ,pixout32[15:8] };
    3'b101: pixout16 <= {pixout32[7:0]  ,pixout32[15:8] };
  endcase

  case ({vga_scale_x,vga_colormode})
    4'b0000: counter_scanout_step <= 3; // 8 bit
    4'b1000: counter_scanout_step <= 7;
    4'b0001: counter_scanout_step <= 1; // 16 bit
    4'b1001: counter_scanout_step <= 3;
    4'b0010: counter_scanout_step <= 0; // 32 bit
    4'b1010: counter_scanout_step <= 1;
    4'b0011: counter_scanout_step <= 1; // 15 bit
    4'b1011: counter_scanout_step <= 3;
  endcase

  /* A viewport with a left border primes scanout at viewport_x - 1. Full-
   * frame content has no such counter value, so use raster wrap as its
   * logical x = -1 cycle and avoid duplicating source column zero. */
  if ((counter_x + 1'b1 < vga_viewport_x ||
       counter_x > vga_viewport_x + vga_viewport_width) &&
      !(counter_x >= vga_h_max && vga_viewport_x == 0)) begin
    counter_scanout  <= 0;
    counter_subpixel <= counter_scanout_step;
  end else begin
    if (counter_subpixel == 0) begin
      counter_subpixel <= counter_scanout_step;
      counter_scanout  <= counter_scanout + 1'b1;
    end else
      counter_subpixel <= counter_subpixel - 1'b1;
  end

  if (vga_colormode==CMODE_16BIT)
    // 16 bit 5r6g5b
    pixout32_dly <= {8'b0,blue16,green16,red16};
  else if (vga_colormode==CMODE_15BIT)
    // 15 bit 5r5g5b for shapeshifter
    pixout32_dly <= {8'b0,blue15,green15,red15};
  else
    pixout32_dly <= pixout32;
  pixout32_dly2 <= pixout32_dly;

  palout <= palette[{vga_selected_palette, pixout8}];

  case (vga_colormode)
    CMODE_8BIT:  pixout <= palout;
    CMODE_16BIT: pixout <= pixout32_dly;
    CMODE_15BIT: pixout <= pixout32_dly;
    CMODE_32BIT: pixout <= pixout32_dly2;
  endcase

  sprite_pix <= sprite_buffer[((sprite_py>>sprite_dbl)<<5)+(sprite_px>>sprite_dbl)];
  if (counter_y >= vga_viewport_y + vga_sprite_y &&
      counter_y < vga_viewport_y + vga_sprite_y2 &&
      counter_x >= vga_viewport_x + vga_sprite_x &&
      counter_x < vga_viewport_x + vga_sprite_x2) begin
    sprite_on <= 1;
    if (sprite_px < (SPRITE_W<<sprite_dbl)-1'b1)
      sprite_px <= sprite_px + 1'b1;
    else begin
      sprite_px <= 0;
      sprite_py <= sprite_py + 1'b1;
    end
  end else begin
    sprite_on <= 0;
  end

  sprite_pix_d1 <= sprite_pix;
  sprite_pix_d2 <= sprite_pix_d1;
  sprite_pix_d3 <= sprite_pix_d2;
  sprite_pix_d4 <= sprite_pix_d3;
  sprite_on_d1 <= sprite_on;
  sprite_on_d2 <= sprite_on_d1;
  sprite_on_d3 <= sprite_on_d2;
  sprite_on_d4 <= sprite_on_d3;

counter_y_d1 <= counter_y;
counter_y_d2 <= counter_y_d1;

if (!vga_scanlines_en || vga_scanline_width == 2'b00) begin
    pixout_sl <= pixout_composited;
end else case (vga_scanline_width)
    2'b01: begin
        // mode 1: 100/0 - one line in two black
        if (scanline_content_y[0] == vga_scanline_parity)
            pixout_sl <= 32'b0;
        else
            pixout_sl <= pixout_composited;
    end
    2'b10: begin
        // mode 2: 100/62 - alternating full / 62.5% (no black lines)
        // 62.5% = >>1 (50%) + >>3 (12.5%)
        if (scanline_content_y[0] == vga_scanline_parity)
            pixout_sl <= pixout_composited;
        else
            pixout_sl <= {8'b0,
                ({1'b0, pixout_composited[23:17]} + {3'b0, pixout_composited[23:19]}),
                ({1'b0, pixout_composited[15:9]}  + {3'b0, pixout_composited[15:11]}),
                ({1'b0, pixout_composited[7:1]}   + {3'b0, pixout_composited[7:3]})
            };
    end
    2'b11: begin
        // mode 3: 100/75/50/75 - soft gradient over 4 lines
        // 75% = >>1 (50%) + >>2 (25%)
        // 50% = >>1
        case (scanline_content_y[1:0] ^ {1'b0, vga_scanline_parity})
            2'b00: pixout_sl <= pixout_composited;
            2'b01: pixout_sl <= {8'b0,
                ({1'b0, pixout_composited[23:17]} + {2'b0, pixout_composited[23:18]}),
                ({1'b0, pixout_composited[15:9]}  + {2'b0, pixout_composited[15:10]}),
                ({1'b0, pixout_composited[7:1]}   + {2'b0, pixout_composited[7:2]})
            };
            2'b10: pixout_sl <= {8'b0,
                1'b0, pixout_composited[23:17],
                1'b0, pixout_composited[15:9],
                1'b0, pixout_composited[7:1]
            };
            2'b11: pixout_sl <= {8'b0,
                ({1'b0, pixout_composited[23:17]} + {2'b0, pixout_composited[23:18]}),
                ({1'b0, pixout_composited[15:9]}  + {2'b0, pixout_composited[15:10]}),
                ({1'b0, pixout_composited[7:1]}   + {2'b0, pixout_composited[7:2]})
            };
        endcase
    end
    default: pixout_sl <= pixout_composited;
endcase

  dvi_rgb <= viewport_output_active ? composed_rgb : 32'b0;

  /* The refill side can overwrite the bank from the completed row during
   * horizontal blanking. Select the upcoming row one clock before raster
   * wrap so that stale-bank data has cleared the registered pixel pipeline
   * before active column zero. */
  if (counter_x + 1'b1 == vga_h_max)
    scanout_source_line_row <= next_scanout_source_line;

  /* These registered coordinates advance on the same edge as counter_x/y.
   * Their values therefore retain the existing pixel/row phase while
   * breaking counter arithmetic away from overlay BRAM/tag/control logic. */
  if (counter_x >= vga_h_max) begin
    overlay_screen_x_position <= overlay_screen_x_origin;
    overlay_local_x_position <= overlay_local_x_origin;
    overlay_read_x_position <= overlay_local_x_origin + 17'sd1;
    viewport_output_x_position <= -$signed(PIPE_DELAY) -
      (vga_overlay_enable ? $signed(OVERLAY_PIPE_DELAY) : 17'sd0);
    viewport_output_y_row <= next_raster_y - vga_scale_y_factor;

    if (counter_y >= vga_v_max) begin
      overlay_screen_y_row <= overlay_screen_y_origin;
      overlay_local_y_row <= overlay_local_y_origin;
      overlay_displayed_line_row <= overlay_local_y_origin >= 0
        ? (vga_overlay_scaling ? vga_overlay_y_start_source[11:0]
                               : overlay_local_y_origin[11:0])
        : 12'b0;
    end else begin
      overlay_screen_y_row <= overlay_screen_y_row + 17'sd1;
      overlay_local_y_row <= overlay_local_y_row + 17'sd1;
      overlay_displayed_line_row <= overlay_local_y_row + 17'sd1 >= 0
        ? (vga_overlay_scaling ? overlay_scale_source_y[11:0]
                               : overlay_local_y_row[11:0] + 1'b1)
        : 12'b0;
    end
  end else begin
    overlay_screen_x_position <= overlay_screen_x_position + 17'sd1;
    overlay_local_x_position <= overlay_local_x_position + 17'sd1;
    overlay_read_x_position <= overlay_read_x_position + 17'sd1;
    viewport_output_x_position <= viewport_output_x_position + 17'sd1;
  end

  if (counter_x >= vga_h_max) begin
    counter_x <= 0;
    if (counter_y >= vga_v_max) begin
      counter_y <= 0;
      sprite_px <= 0;
      sprite_py <= 0;
    end else begin
      counter_y <= counter_y + 1'b1;
    end
  end else begin
    counter_x <= counter_x + 1'b1;
  end

  /* The viewport is stable for a frame, and these stages have an entire
   * raster row to settle before h_rez publishes the next VDMA line request.
   * Keeping the subtraction, upper-bound addition/comparison, and final mux
   * in separate cycles avoids one long counter_y -> need_line_fetch path
   * without moving the established request edge. */
  need_line_fetch_candidate <= counter_y - vga_viewport_y + 1'b1;
  need_line_fetch_upper_bound <=
    vga_viewport_y + vga_viewport_height - 1'b1;
  need_line_fetch_lower_valid <= counter_y >= vga_viewport_y;
  need_line_fetch_row_valid <= need_line_fetch_lower_valid &&
    counter_y < need_line_fetch_upper_bound;

  if (counter_x==vga_h_rez)
    need_line_fetch <= need_line_fetch_row_valid
      ? need_line_fetch_candidate : 12'b0;

  /* Display selection follows the current PIP row while this independent
   * fetch selector advances as soon as that row is known ready. MM2S can then
   * fill the opposite BRAM bank throughout the current raster row instead of
   * having only horizontal blanking after the final PIP pixel. Requiring the
   * displayed line to be ready prevents a missed deadline from skipping an
   * unfetched source row. */
  if (!vga_overlay_enable) begin
    overlay_fetch_line <= 0;
  end else if (overlay_accepted_generation !=
               vga_overlay_frame_generation) begin
    /* Firmware advances the generation only after committing the VDMA
     * address/VSIZE at vblank. The line-buffer's atomic acknowledgement
     * proves its AXI domain has stopped treating the old frame as active;
     * only then may the pixel domain request the first visible source line. */
    vga_overlay_frame_generation <= overlay_accepted_generation;
    overlay_applied_scale_epoch <= vga_overlay_scale_epoch;
    overlay_fetch_line <= vga_overlay_scaling
      ? vga_overlay_y_start_source[11:0] : 12'b0;
    overlay_fetch_request <= 1;
  end else if (overlay_applied_scale_epoch !=
               vga_overlay_scale_epoch) begin
    /* Geometry can change while a simulation or legacy producer retains the
     * same frame generation. Restart at the correctly clipped source row. */
    overlay_applied_scale_epoch <= vga_overlay_scale_epoch;
    overlay_fetch_line <= vga_overlay_scaling
      ? vga_overlay_y_start_source[11:0] : 12'b0;
    overlay_fetch_request <= 1;
  end else if (!overlay_scheduler_scaling &&
               overlay_scheduler_y_in_window &&
               overlay_scheduler_line_ready &&
               overlay_fetch_line == overlay_scheduler_line) begin
    if ({4'b0, overlay_scheduler_line} + 16'd1 < vga_overlay_height) begin
      overlay_fetch_line <= overlay_scheduler_line + 1'b1;
      overlay_fetch_request <= 1;
    end
  end else if (overlay_scheduler_scaling &&
               overlay_scheduler_screen_y_nonnegative &&
               overlay_scheduler_y_in_window &&
               overlay_scheduler_line_ready &&
               overlay_fetch_line == overlay_scheduler_line &&
               overlay_scheduler_next_source_y != overlay_scheduler_line) begin
    overlay_fetch_line <= overlay_scheduler_next_source_y;
    overlay_fetch_request <= 1;
  end

  // signal synchronization point to fetch process
  if (counter_x<8 && counter_y==vga_v_sync_start)
    need_frame_sync <= 1;
  else
    need_frame_sync <= 0;

  // rasterline interrupt:
  // - first time on vblank start (1 pixel long)
  // - second time on report_y (1 pixel long)
  if (counter_y == vga_v_sync_start || (vga_report_y != 0 && (counter_y == vga_report_y - 1'b1))) begin
    // i tested the position of the interrupt relative to vdma_init,
    // there's a wide window where a buffer switch is ok, and
    // another window in which we get a line that flickers in the middle.
    if (counter_x == vga_h_rez)
      control_vblank[1] <= 1;
    else
      control_vblank[1] <= 0;
  end

  // internal vblank signal
  if (counter_y >= vga_v_rez && counter_y < vga_v_max) begin
    control_vblank[0] <= 1;
    // propagate report (interrupt) line position in vblank
    // to avoid glitches
    vga_report_y <= vga_report_y_next;
  end else begin
    control_vblank[0] <= 0;
  end

  // 4 clocks pipeline delay
  // VESA DPMS disables a sync by holding it at the inactive level. Keep the
  // raster/vblank state machines running so firmware and P96 waits continue
  // to work while the monitor is asleep.
  if (vga_dpms_level == DPMS_STANDBY || vga_dpms_level == DPMS_OFF)
    dvi_hsync <= 0^vga_sync_polarity;
  else if (counter_x >= vga_h_sync_start_delayed && counter_x < vga_h_sync_end_delayed)
    dvi_hsync <= 1^vga_sync_polarity;
  else
    dvi_hsync <= 0^vga_sync_polarity;

  if (vga_dpms_level == DPMS_SUSPEND || vga_dpms_level == DPMS_OFF)
    dvi_vsync <= 0^vga_sync_polarity;
  else if (counter_x >= vga_h_sync_start_delayed)
    if (counter_y >= vga_v_sync_start && counter_y < vga_v_sync_end)
      dvi_vsync <= 1^vga_sync_polarity;
    else
      dvi_vsync <= 0^vga_sync_polarity;

  // account for 1 line of vdma wrap-around
  if (counter_y >= vga_scale_y_factor &&
      counter_y < (vga_v_rez + vga_scale_y_factor) &&
      counter_x == PIPE_DELAY +
                   (vga_overlay_enable ? OVERLAY_PIPE_DELAY : 0))
    dvi_active_video <= 1;

  if (counter_x==vga_h_rez_delayed)
    dvi_active_video <= 0;
end

endmodule
