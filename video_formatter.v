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
  (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF m_axis_vid:overlay_axis" *)
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
  // capture anchor toggle (cap_clk domain; one edge per input field/frame,
  // at the first completed post-crop full-width source row)
  input capture_anchor_toggle,
  output reg [1:0]control_vblank,
  // source-sync diagnostic snapshot (m_axis_vid_aclk domain): coherent
  // 64-bit copy of video_source_sync's diagnostic_data, refreshed once
  // per output frame - bit map documented in video_source_sync.v
  output reg [63:0] source_sync_diagnostic,
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
localparam OP_SOURCE_SYNC=30; // output frame follows the capture anchor (bit0: enable)

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
reg [11:0] scale_source_rows = 0;
reg [23:0] palette[511:0];
reg [2:0] colormode = CMODE_32BIT;
reg vsync_request;
reg sync_polarity = 1; // negative polarity
reg selected_palette = 0;
reg [1:0] dpms_level = DPMS_ON;
reg source_sync_enable = 0; // aclk domain; OP_SOURCE_SYNC bit0, CDC'd below

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

/* Scaling uses 16.16 fixed-point source positions. After a geometry change
 * a small restoring divider computes step = (source << 16) / destination per
 * axis, then the position of the first visible output pixel, centre-aligned:
 * (d + 0.5) * step - 0.5 at d = clip. Scanout then needs one add per pixel
 * and per row. */
reg [15:0] overlay_calc_x = 0;
reg [15:0] overlay_calc_y = 0;
reg [15:0] overlay_calc_width = 0;
reg [15:0] overlay_calc_height = 0;
reg [15:0] overlay_calc_source_width = 0;
reg [15:0] overlay_calc_source_height = 0;
reg [15:0] overlay_calc_clip_x = 0;
reg [15:0] overlay_calc_clip_y = 0;
reg [31:0] overlay_x_step = 0;
reg [31:0] overlay_y_step = 0;
reg [47:0] overlay_x_clip_product = 0;
reg [47:0] overlay_y_clip_product = 0;
reg signed [31:0] overlay_x_start = 0;
reg signed [31:0] overlay_y_start = 0;
/* Last luma pixel, last macropixel word and last row of the source: the
 * bilinear neighbours clamp there. */
reg [11:0] overlay_luma_max = 0;
reg [11:0] overlay_word_max = 0;
reg [11:0] overlay_row_max = 0;
reg overlay_scale_ready = 0;
reg [31:0] overlay_scale_epoch = 0;

reg overlay_div_busy = 0;
reg overlay_div_kind = 0;
reg [1:0] overlay_start_stage = 0;
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

// VGA timing consumers are 12-bit; the control words carry them in [27:16]
// and [11:0], so the upper nibbles were never observable.
reg [11:0] screen_h_max;
reg [11:0] screen_v_max;
reg [11:0] screen_h_sync_start;
reg [11:0] screen_h_sync_end;
reg [11:0] screen_v_sync_start;
reg [11:0] screen_v_sync_end;

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
reg [10:0] sprite_addr_in; // sprite_buffer has 1536 = 2^11 entries
reg [11:0] sprite_x;
reg [11:0] sprite_y;
reg sprite_dbl;
reg [11:0] report_y = 0;
reg [11:0] vga_sprite_x; // vga domain
reg [11:0] vga_sprite_y; // vga domain
reg [11:0] vga_sprite_x2; // vga domain
reg [11:0] vga_sprite_y2; // vga domain
reg [11:0] sprite_px; // vga domain
reg [11:0] sprite_py; // vga domain
reg [23:0] sprite_pix; // vga domain
reg sprite_on; // vga domain
reg [11:0] vga_report_y; // vga domain
reg [11:0] vga_report_y_next; // vga domain
reg vga_selected_palette; // vga domain
reg [1:0]  vga_scanline_width;
reg        vga_scanline_parity;
reg vga_scanlines_en;
reg [31:0] pixout_sl;
// Only the two low scanline-parity bits of counter_y are consumed (see
// scanline_content_y); pipeline just those bits.
reg [1:0] counter_y_d1;
reg [1:0] counter_y_d2;

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
    need_line_fetch_reg2 <= scale_source_rows != 0
      ? need_line_fetch_reg : need_line_fetch_reg >> scale_y_effective;
    need_line_fetch_reg3 <= need_line_fetch_reg2;

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
          // reading from vdma; request changes remain pending until EOL

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
          else if (need_line_fetch_reg2 == need_line_fetch_reg3 &&
                   need_line_fetch_reg3 != last_line_fetch) begin
            // time to read the next line after a coherent CDC sample
            next_input_state <= 4'h1;
            last_line_fetch <= need_line_fetch_reg3;
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

/* Source-sync control crosses to the pixel domain like the other
 * quasi-static control bits.  The anchor arrives from the capture
 * clock, which is not present here, so it is only double-flop
 * synchronized (SRC_INPUT_REG=0, no source register stage). */
wire source_sync_enable_pix;
wire capture_anchor_pix;
xpm_cdc_single #(
  .DEST_SYNC_FF(3),
  .INIT_SYNC_FF(1),
  .SIM_ASSERT_CHK(0),
  .SRC_INPUT_REG(1)
) source_sync_enable_cdc (
  .src_clk(m_axis_vid_aclk),
  .src_in(source_sync_enable),
  .dest_clk(dvi_clk),
  .dest_out(source_sync_enable_pix)
);
xpm_cdc_single #(
  .DEST_SYNC_FF(3),
  .INIT_SYNC_FF(1),
  .SIM_ASSERT_CHK(0),
  .SRC_INPUT_REG(0)
) capture_anchor_cdc (
  .src_clk(1'b0), // no source clock available; unused with SRC_INPUT_REG=0
  .src_in(capture_anchor_toggle),
  .dest_clk(dvi_clk),
  .dest_out(capture_anchor_pix)
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
        scale_source_rows <= control_data_in[27:16];
        sprite_dbl <= control_data_in[3];
      end
    OP_COLORMODE: colormode  <= control_data_in[1:0]; // FIXME
    OP_VSYNC: vsync_request <= 1; //control_data[0];
    OP_MAX: begin
        screen_v_max <= control_data_in[27:16];
        screen_h_max <= control_data_in[11:0];
      end
    OP_HS: begin
        screen_h_sync_start <= control_data_in[27:16];
        screen_h_sync_end <= control_data_in[11:0];
      end
    OP_VS: begin
        screen_v_sync_start <= control_data_in[27:16];
        screen_v_sync_end <= control_data_in[11:0];
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
        sprite_addr_in <= control_data_in[10:0];
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
    OP_SOURCE_SYNC: source_sync_enable <= control_data_in[0];
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
    overlay_calc_clip_x <= 0;
    overlay_calc_clip_y <= 0;
    overlay_x_step <= 0;
    overlay_y_step <= 0;
    overlay_x_clip_product <= 0;
    overlay_y_clip_product <= 0;
    overlay_x_start <= 0;
    overlay_y_start <= 0;
    overlay_luma_max <= 0;
    overlay_word_max <= 0;
    overlay_row_max <= 0;
    overlay_scale_ready <= 0;
    overlay_scale_epoch <= 0;
    overlay_div_busy <= 0;
    overlay_div_kind <= 0;
    overlay_start_stage <= 0;
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
    overlay_calc_clip_x <= overlay_clip_x;
    overlay_calc_clip_y <= overlay_clip_y;
    overlay_luma_max <= overlay_source_width[11:0] - 1'b1;
    overlay_word_max <= overlay_source_width[12:1] + overlay_source_width[0]
                        - 1'b1;
    overlay_row_max <= overlay_source_height[11:0] - 1'b1;
    overlay_scale_ready <= 0;
    overlay_start_stage <= 0;
    overlay_div_busy <= overlay_width != 0 &&
                        overlay_height != 0 &&
                        overlay_source_width != 0 &&
                        overlay_source_height != 0;
    overlay_div_kind <= 0;
    overlay_div_bit <= 31;
    overlay_dividend <= {overlay_source_width, 16'b0};
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
      if (!overlay_div_kind) begin
        overlay_x_step <= overlay_div_result_quotient;
        overlay_div_kind <= 1;
        overlay_dividend <= {overlay_calc_source_height, 16'b0};
        overlay_divisor <= overlay_calc_height;
      end else begin
        overlay_y_step <= overlay_div_result_quotient;
        overlay_div_busy <= 0;
        overlay_start_stage <= 1;
      end
    end
  end else if (overlay_start_stage == 1) begin
    /* clip * step fits 32 bits for any clip inside the destination:
     * it is the source position of the first visible pixel. */
    overlay_x_clip_product <= overlay_calc_clip_x * overlay_x_step;
    overlay_y_clip_product <= overlay_calc_clip_y * overlay_y_step;
    overlay_start_stage <= 2;
  end else if (overlay_start_stage == 2) begin
    overlay_x_start <= $signed(overlay_x_clip_product[31:0]) +
                       $signed({1'b0, overlay_x_step[31:1]}) -
                       32'sd32768;
    overlay_y_start <= $signed(overlay_y_clip_product[31:0]) +
                       $signed({1'b0, overlay_y_step[31:1]}) -
                       32'sd32768;
    overlay_start_stage <= 0;
    overlay_scale_ready <= 1;
    overlay_scale_epoch <= overlay_scale_epoch + 1'b1;
  end
end

localparam PIPE_DELAY = 4;
/* Cycles from the registered source-position stage to the YUV converter:
 * source-position stage (1), registered BRAM address (1), BRAM output
 * register (1), and the eight-stage bilinear filter. */
localparam OVERLAY_FILTER_DELAY = 11;
/* The converter's registered clamp/output stage adds five cycles. */
localparam OVERLAY_PIPE_DELAY = 5 + OVERLAY_FILTER_DELAY;
/* Sprite pixels delayed to match the overlay pipeline. */
reg [24*OVERLAY_PIPE_DELAY-1:0] sprite_pix_delay = 0;
reg [OVERLAY_PIPE_DELAY-1:0] sprite_on_delay = 0;
wire [23:0] sprite_pix_overlay =
  sprite_pix_delay[24*OVERLAY_PIPE_DELAY-1 -: 24];
wire sprite_on_overlay = sprite_on_delay[OVERLAY_PIPE_DELAY-1];

reg [31:0] palout;
reg [11:0] vga_v_rez;
reg [11:0] vga_h_rez;
reg [11:0] vga_h_rez_plus_pipe = 0;
reg [11:0] vga_h_rez_delayed;
reg [11:0] vga_v_max;
reg [11:0] vga_h_max;
reg [11:0] fractional_line_update_x = 0;
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
reg viewport_has_left_border = 0;
reg [11:0] viewport_scanout_first = 0;
reg [12:0] viewport_scanout_last = 0;
reg viewport_initialized = 0;
reg viewport_geometry_ready = 0;

reg vga_scale_x = 0;
reg [1:0] vga_scale_y = 2'd0;
reg [11:0] vga_scale_source_rows = 0;
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
reg [31:0] vga_overlay_x_step = 0;
reg [31:0] vga_overlay_y_step = 0;
reg signed [31:0] vga_overlay_x_start = 0;
reg signed [31:0] vga_overlay_y_start = 0;
reg [11:0] vga_overlay_luma_max = 0;
reg [11:0] vga_overlay_word_max = 0;
reg [11:0] vga_overlay_row_max = 0;
reg [31:0] vga_overlay_scale_epoch = 0;
reg [31:0] overlay_applied_scale_epoch = 0;
reg [23:0] vga_overlay_key_rgb = 0;
reg [31:0] vga_overlay_frame_generation = 0;
reg [11:0] overlay_fetch_line = 0;
reg overlay_fetch_request = 0;
/* 16.16 source positions. overlay_pos_x belongs to the pixel whose line
 * buffer read is presented in this cycle; overlay_pos_y to the raster row
 * after the current one once h_rez has passed. */
reg signed [31:0] overlay_pos_x = 0;
reg signed [31:0] overlay_pos_y = 0;
/* Source positions are staged before address/weight generation so the
 * 16.16 floor, edge and parity logic cannot sit on the BRAM address path. */
reg signed [31:0] overlay_pos_x_staged = 0;
reg signed [31:0] overlay_pos_x_start = 0;
reg signed [31:0] overlay_pos_x_step = 0;
reg signed [31:0] overlay_pos_y_start = 0;
reg signed [31:0] overlay_pos_y_step = 0;

/* Coordinate state advances with the raster counters, but is registered
 * separately so overlay window/tag arithmetic never sits on the same
 * 150 MHz path as the fetch scheduler or BRAM bank selection. */
reg signed [16:0] overlay_screen_x_position = 0;
reg signed [16:0] overlay_local_x_position = 0;
reg signed [16:0] overlay_screen_x_origin = 0;
reg signed [16:0] overlay_local_x_origin = 0;
reg signed [16:0] overlay_screen_y_origin = 0;
reg signed [16:0] overlay_local_y_origin = 0;
reg signed [16:0] overlay_screen_y_row = 0;
reg signed [16:0] overlay_local_y_row = 0;
/* The two source rows the current raster row blends, and the weight of the
 * lower one. */
reg [11:0] overlay_displayed_line_row = 0;
reg [11:0] overlay_displayed_next_row = 0;
reg [7:0] overlay_row_weight = 0;
reg signed [16:0] overlay_visible_x1_unclipped_pre = 0;
reg signed [16:0] overlay_visible_x0_minus_one = -1;
reg signed [16:0] overlay_visible_x1_minus_one = -1;
reg signed [16:0] viewport_output_x_position = 0;
reg [11:0] viewport_output_y_row = 0;
reg [11:0] scanout_source_line_row = 0;
reg [12:0] viewport_output_x_end = 0;
reg overlay_scheduler_y_in_window = 0;
reg overlay_scheduler_screen_y_nonnegative = 0;
/* Window rows that fall into vertical blanking are never displayed; a
 * window clipped at the screen bottom must not count them as consumed. */
reg overlay_scheduler_row_visible = 0;

/* Fetch-side row counter. The bilinear filter reads source rows r and r+1
 * for every output row, and the native plane scales down at most 2x, so
 * every source row from the window's first to the frame's last is needed:
 * the fetcher issues them in order as soon as the line fetcher has a free
 * bank, running ahead of the display. */
reg [11:0] overlay_fetch_row = 0;
reg overlay_fetch_issued = 0;
reg overlay_fetch_active = 0;
/* Set when the raster leaves the visible frame; the next generation clears
 * it. No request may be in flight across the vblank buffer flip. */
reg overlay_frame_fetch_done = 0;
/* Top source row on display in the window; banks holding older rows are
 * free. */
reg [11:0] overlay_consumed_line = 0;
reg overlay_consumed_valid = 0;
wire overlay_fetch_ready;
wire overlay_fetch_passed = overlay_consumed_valid &&
  overlay_fetch_row < overlay_consumed_line;

wire signed [16:0] overlay_screen_x = overlay_screen_x_position;
wire signed [16:0] overlay_screen_y = overlay_screen_y_row;
wire signed [16:0] overlay_local_x = overlay_local_x_position;
wire signed [16:0] overlay_local_y = overlay_local_y_row;
wire overlay_in_window = vga_overlay_enable &&
  overlay_local_x >= 0 && overlay_local_y >= 0 &&
  overlay_local_x < $signed({1'b0, vga_overlay_width}) &&
  overlay_local_y < $signed({1'b0, vga_overlay_height});
wire [11:0] overlay_displayed_line = overlay_displayed_line_row;

/* Integer part of a non-negative 16.16 position, saturated to 12 bits. */
function [11:0] overlay_floor12;
  input signed [31:0] pos;
  begin
    if (pos < 0)
      overlay_floor12 = 12'd0;
    else if (pos[30:28] != 3'd0)
      overlay_floor12 = 12'hfff;
    else
      overlay_floor12 = pos[27:16];
  end
endfunction

/* Horizontal sample geometry of the pixel being read. Luma blends pixels
 * i and i+1; chroma, co-sited with the even luma, blends macropixels k and
 * k+1 at position pos/2. i lies in macropixel k and i+1 in k or k+1, so one
 * even and one odd word per row cover every sample. Both clamp at the
 * source edge with weight zero. */
wire signed [31:0] overlay_pos_c = overlay_pos_x_staged >>> 1;
wire [11:0] overlay_luma_floor = overlay_floor12(overlay_pos_x_staged);
wire [11:0] overlay_word_floor = overlay_floor12(overlay_pos_c);
wire overlay_luma_edge = overlay_luma_floor >= vga_overlay_luma_max;
wire overlay_word_edge = overlay_word_floor >= vga_overlay_word_max;
wire [11:0] overlay_luma_i =
  overlay_luma_edge ? vga_overlay_luma_max : overlay_luma_floor;
wire [11:0] overlay_word_k =
  overlay_word_edge ? vga_overlay_word_max : overlay_word_floor;
wire [7:0] overlay_luma_weight =
  (overlay_pos_x_staged < 0 || overlay_luma_edge) ? 8'd0 :
  overlay_pos_x_staged[15:8];
wire [7:0] overlay_chroma_weight =
  (overlay_pos_c < 0 || overlay_word_edge) ? 8'd0 : overlay_pos_c[15:8];
/* Word k < 1280, so k / 2 fits the line buffer's 10-bit half-row address. */
wire [9:0] overlay_read_addr_odd_next = overlay_word_k[10:1];
wire [9:0] overlay_read_addr_even_next =
  overlay_word_k[10:1] + (overlay_word_k[0] && !overlay_word_edge);
/* The addresses and the filter's sample geometry are registered, so the
 * BRAM address pins see only routing. */
reg [9:0] overlay_read_addr_odd = 0;
reg [9:0] overlay_read_addr_even = 0;
reg overlay_sample_luma_odd = 0;
reg overlay_sample_luma_edge = 0;
reg [7:0] overlay_sample_luma_weight = 0;
reg overlay_sample_word_odd = 0;
reg overlay_sample_word_edge = 0;
reg [7:0] overlay_sample_chroma_weight = 0;
always @(posedge dvi_clk) begin
  overlay_pos_x_staged <= overlay_pos_x;

  overlay_read_addr_odd <= overlay_read_addr_odd_next;
  overlay_read_addr_even <= overlay_read_addr_even_next;
  overlay_sample_luma_odd <= overlay_luma_i[0];
  overlay_sample_luma_edge <= overlay_luma_edge;
  overlay_sample_luma_weight <= overlay_luma_weight;
  overlay_sample_word_odd <= overlay_word_k[0];
  overlay_sample_word_edge <= overlay_word_edge;
  overlay_sample_chroma_weight <= overlay_chroma_weight;
end

/* Vertical sample geometry {r, r + 1, weight} of a 16.16 row position. */
function [31:0] overlay_row_sample;
  input signed [31:0] pos;
  input [11:0] last;
  reg [11:0] floor_row;
  reg [11:0] next_row;
  begin
    floor_row = overlay_floor12(pos);
    next_row = floor_row + 1'b1;
    if (floor_row >= last)
      overlay_row_sample = {last, last, 8'd0};
    else
      overlay_row_sample = {floor_row, next_row,
                            pos < 0 ? 8'd0 : pos[15:8]};
  end
endfunction

/* Both candidates for the next raster row are registered ahead of the line
 * wrap, which only picks one: the first row of a frame starts at the
 * clipped start position, every other row at overlay_pos_y (advanced at
 * h_rez, well before the wrap). */
reg [31:0] overlay_row_start_sample = 0;
reg [31:0] overlay_row_next_sample = 0;
always @(posedge dvi_clk) begin
  overlay_row_start_sample <=
    overlay_row_sample(vga_overlay_y_start, vga_overlay_row_max);
  overlay_row_next_sample <=
    overlay_row_sample(overlay_pos_y, vga_overlay_row_max);
end
/* The fetcher starts at the first visible row's top source row. */
wire [11:0] overlay_fetch_start_floor = overlay_floor12(vga_overlay_y_start);
wire [11:0] overlay_fetch_first_row =
  overlay_fetch_start_floor >= vga_overlay_row_max
    ? vga_overlay_row_max : overlay_fetch_start_floor;

wire [31:0] overlay_read_top_even;
wire [31:0] overlay_read_top_odd;
wire [31:0] overlay_read_bottom_even;
wire [31:0] overlay_read_bottom_odd;
wire [31:0] overlay_yuv422;
wire overlay_line_ready;
wire [31:0] overlay_accepted_generation;
wire [11:0] overlay_scheduler_line;

/* Source-synchronous frame timing.  The controller sees the live dvi
 * domain geometry and the raster position, and decides once per line,
 * before the line begins, whether that line ends the output frame.
 * While disabled (or disengaged) frame_wrap_this_line is exactly the
 * original free-running compare, so fixed modes and RTG are unchanged
 * bit for bit; every vertical wrap consumer below is muxed on this one
 * wire, which is why the hmax-1 line-bank prefetch decision and the
 * actual counter_y wrap can never disagree. */
/* Quasi-static geometry.  A combinational 13-bit add here lands on the
 * 150 MHz compare that clocks good_intervals and misses timing on the
 * 2MB Zorro II placement.  One pixel of delay is inside the frame-boundary
 * bounds pipeline. */
reg [12:0] source_sync_active_end = 13'd0;
always @(posedge dvi_clk) begin
  if (!aresetn)
    source_sync_active_end <= 13'd0;
  else
    source_sync_active_end <= {1'b0, vga_v_rez} + {1'b0, vga_scale_y_factor};
end
wire source_sync_line_advance = counter_x >= vga_h_max;
wire [63:0] source_sync_diag_bus;
wire source_sync_line_uses_sync;
wire source_sync_line_is_last;
wire source_sync_video_hidden;

video_source_sync video_source_sync_i (
  .dvi_clk(dvi_clk),
  .resetn(aresetn),
  .enable(source_sync_enable_pix),
  .anchor_toggle(capture_anchor_pix),
  .nominal_last_line(vga_v_max),
  .vsync_end_line(vga_v_sync_end),
  .active_end_line(source_sync_active_end),
  .line_last_pixel(vga_h_max),
  .interlace(control_interlace),
  .raster_y(counter_y),
  .line_advance(source_sync_line_advance),
  .line_uses_sync(source_sync_line_uses_sync),
  .line_is_last(source_sync_line_is_last),
  .video_hidden(source_sync_video_hidden),
  .diagnostic_data(source_sync_diag_bus)
);

wire frame_wrap_this_line = source_sync_line_uses_sync
  ? source_sync_line_is_last
  : counter_y >= vga_v_max;
wire [11:0] next_raster_y = frame_wrap_this_line
  ? 12'b0 : counter_y + 1'b1;
reg [11:0] next_raster_y_staged = 0;
wire [11:0] staged_next_scanout_content_y =
  next_raster_y_staged - vga_viewport_y;
wire [11:0] power_of_two_next_source_line =
  (next_raster_y_staged >= vga_viewport_y + vga_scale_y_factor)
    ? ((staged_next_scanout_content_y - vga_scale_y_factor) >> vga_scale_y)
    : 12'b0;

/* Full-width NTSC has 200 progressive (400 woven) source rows, which do
 * not divide the 1024-row output by a power of two.  Advance at most one
 * source row per displayed row with an exact line-rate accumulator. */
wire fractional_scale_y = vga_scale_source_rows != 0;
reg [12:0] fractional_content_start = 0;
reg [12:0] fractional_content_end = 0;
reg [11:0] fractional_source_line = 0;
reg [12:0] fractional_y_error = 0;
wire [12:0] fractional_y_sum =
  fractional_y_error + {1'b0, vga_scale_source_rows};
wire fractional_y_advance =
  fractional_y_sum >= {1'b0, vga_viewport_height};
wire [11:0] fractional_next_source_line =
  fractional_source_line + fractional_y_advance;
wire [12:0] fractional_next_y_error = fractional_y_advance
  ? fractional_y_sum - {1'b0, vga_viewport_height}
  : fractional_y_sum;
wire fractional_next_row_active =
  {1'b0, next_raster_y_staged} >= fractional_content_start &&
  {1'b0, next_raster_y_staged} < fractional_content_end;
wire fractional_next_row_advances =
  {1'b0, next_raster_y_staged} > fractional_content_start &&
  {1'b0, next_raster_y_staged} < fractional_content_end;
reg fractional_next_row_advances_latched = 0;
wire fractional_current_row_active =
  {1'b0, counter_y} >= fractional_content_start &&
  {1'b0, counter_y} < fractional_content_end;
wire [12:0] fractional_prefetch_source_line =
  {1'b0, fractional_source_line} + 1'b1;
reg fractional_current_row_active_latched = 0;
reg [11:0] fractional_prefetch_source_line_latched = 0;
reg fractional_prefetch_valid_latched = 0;
wire [11:0] next_scanout_source_line =
  fractional_scale_y
    ? ({1'b0, next_raster_y_staged} == fractional_content_start
        ? 12'b0
        : (fractional_next_row_active
            ? fractional_next_source_line : 12'b0))
    : power_of_two_next_source_line;
reg [11:0] next_scanout_source_line_latched = 0;

/* Pipeline the viewport bounds and next-row decision: deriving them on the
 * line-wrap edge otherwise puts multiple carry chains in the 150 MHz
 * accumulator reset path. */
always @(posedge dvi_clk) begin
  if (!aresetn) begin
    fractional_content_start <= 0;
    fractional_content_end <= 0;
    next_raster_y_staged <= 0;
    next_scanout_source_line_latched <= 0;
    fractional_next_row_advances_latched <= 0;
    fractional_current_row_active_latched <= 0;
    fractional_prefetch_source_line_latched <= 0;
    fractional_prefetch_valid_latched <= 0;
  end else begin
    fractional_content_start <=
      {1'b0, vga_viewport_y} + {1'b0, vga_scale_y_factor};
    fractional_content_end <=
      fractional_content_start + {1'b0, vga_viewport_height};
    if (counter_x == 0) begin
      /* Stage the wrap decision before the accumulator/comparison stage.
       * This keeps vga_v_max and source-sync wrap logic off the 150 MHz
       * scanout-row update path. */
      next_raster_y_staged <= next_raster_y;
      fractional_current_row_active_latched <=
        fractional_current_row_active;
      fractional_prefetch_source_line_latched <=
        fractional_prefetch_source_line[11:0];
    end else if (counter_x == 1) begin
      fractional_next_row_advances_latched <=
        fractional_next_row_advances;
      next_scanout_source_line_latched <= next_scanout_source_line;
      fractional_prefetch_valid_latched <=
        fractional_current_row_active_latched &&
        {1'b0, fractional_prefetch_source_line_latched} <
          {1'b0, vga_scale_source_rows};
    end
  end
end

always @(posedge dvi_clk) begin
  if (!aresetn || !fractional_scale_y) begin
    fractional_source_line <= 0;
    fractional_y_error <= 0;
  end else if (counter_x == fractional_line_update_x) begin
    if (!fractional_next_row_advances_latched) begin
      fractional_source_line <= 0;
      fractional_y_error <= 0;
    end else begin
      fractional_source_line <= fractional_next_source_line;
      fractional_y_error <= fractional_next_y_error;
    end
  end
end

/* ------------------------------------------------------------------ */
/* Source-sync diagnostic transport (dvi_clk -> m_axis_vid_aclk)       */
/* ------------------------------------------------------------------ */

/* Purely observational side channel: publishes the synchronizer's
 * 64-bit diagnostic bus (bit map in video_source_sync.v) to the ARM
 * domain once per output frame.  The source half arms on the
 * frame-wrap line_advance cycle (the exact predicate the raster
 * counter wraps on) and snapshots the bus one dvi_clk later: the
 * controller updates its wrap metrics (anchor age, frame total,
 * max-forced) with nonblocking assignments on the wrap edge itself,
 * so sampling in that same cycle would publish the previous frame's
 * metrics.  The snapshot register then stays stable for the entire
 * XPM handshake; the destination half publishes a value only when a
 * completed transfer arrives and then holds it until the next one, so
 * a stopped pixel clock simply freezes the last coherent sample.  The
 * transport is never gated on enable/lock/picture state or on any
 * firmware action, so diagnostics stay visible while the synchronizer
 * is disabled (fixed50/RTG fallback) or disengaged. */
localparam [1:0] SS_DIAG_IDLE   = 2'd0;
localparam [1:0] SS_DIAG_LOAD   = 2'd1;
localparam [1:0] SS_DIAG_SEND   = 2'd2;
localparam [1:0] SS_DIAG_RETURN = 2'd3;

reg [1:0]  source_sync_diag_state = SS_DIAG_IDLE;
reg [63:0] source_sync_diag_src_payload = 64'd0;
reg        source_sync_diag_src_send = 1'b0;
wire       source_sync_diag_src_rcv;
wire [63:0] source_sync_diag_dest_payload;
wire       source_sync_diag_dest_req;
reg        source_sync_diag_dest_ack = 1'b0;

xpm_cdc_handshake #(
  .DEST_EXT_HSK(1),
  .DEST_SYNC_FF(3),
  .INIT_SYNC_FF(1),
  .SIM_ASSERT_CHK(0),
  .SRC_SYNC_FF(3),
  .WIDTH(64)
) source_sync_diag_cdc (
  .src_clk(dvi_clk),
  .src_in(source_sync_diag_src_payload),
  .src_send(source_sync_diag_src_send),
  .src_rcv(source_sync_diag_src_rcv),
  .dest_clk(m_axis_vid_aclk),
  .dest_out(source_sync_diag_dest_payload),
  .dest_req(source_sync_diag_dest_req),
  .dest_ack(source_sync_diag_dest_ack)
);

/* Source half (dvi_clk), mirroring the viewport control CDC state
 * machine in the opposite direction: the payload register is loaded
 * once per frame wrap and then never touched until the handshake has
 * fully returned. */
always @(posedge dvi_clk) begin
  if (!aresetn) begin
    source_sync_diag_state <= SS_DIAG_IDLE;
    source_sync_diag_src_payload <= 64'd0;
    source_sync_diag_src_send <= 1'b0;
  end else begin
    case (source_sync_diag_state)
      SS_DIAG_IDLE:
        if (source_sync_line_advance && frame_wrap_this_line)
          source_sync_diag_state <= SS_DIAG_LOAD;
      /* Latch the post-update wrap metrics one cycle after the wrap
       * edge; raising src_send in the same edge keeps the payload
       * stable for the whole handshake. */
      SS_DIAG_LOAD: begin
        source_sync_diag_src_payload <= source_sync_diag_bus;
        source_sync_diag_src_send <= 1'b1;
        source_sync_diag_state <= SS_DIAG_SEND;
      end
      SS_DIAG_SEND:
        if (source_sync_diag_src_rcv) begin
          source_sync_diag_src_send <= 1'b0;
          source_sync_diag_state <= SS_DIAG_RETURN;
        end
      SS_DIAG_RETURN:
        if (!source_sync_diag_src_rcv)
          source_sync_diag_state <= SS_DIAG_IDLE;
    endcase
  end
end

/* Destination half (m_axis_vid_aclk): latch-and-ack like the viewport
 * installer, but immediate - debug data must never wait for a frame
 * boundary.  source_sync_diagnostic (the module output) only ever
 * changes on a completed transfer and is a plain register, so the ARM
 * side always reads one coherent 64-bit snapshot. */
always @(posedge m_axis_vid_aclk) begin
  if (!aresetn) begin
    source_sync_diag_dest_ack <= 1'b0;
    source_sync_diagnostic <= 64'd0;
  end else begin
    if (!source_sync_diag_dest_req)
      source_sync_diag_dest_ack <= 1'b0;
    else if (!source_sync_diag_dest_ack) begin
      source_sync_diagnostic <= source_sync_diag_dest_payload;
      source_sync_diag_dest_ack <= 1'b1;
    end
  end
end

/* Eight banks: the two rows on display and up to six ahead. A bilinear
 * downscale consumes up to two new rows per raster line, so the lookahead
 * must absorb those bursts from a stream that delivers one row at a time. */
video_overlay_linebuffer #(.BANK_BITS(3)) overlay_linebuffer (
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
  .read_addr_even(overlay_read_addr_even),
  .read_addr_odd(overlay_read_addr_odd),
  .displayed_line(overlay_displayed_line),
  .displayed_line_next(overlay_displayed_next_row),
  .read_top_even(overlay_read_top_even),
  .read_top_odd(overlay_read_top_odd),
  .read_bottom_even(overlay_read_bottom_even),
  .read_bottom_odd(overlay_read_bottom_odd),
  .displayed_line_ready(overlay_line_ready),
  .scheduler_line(overlay_scheduler_line),
  .fetch_ready(overlay_fetch_ready),
  .consumed_line(overlay_consumed_line),
  .consumed_valid(overlay_consumed_valid),
  .accepted_generation(overlay_accepted_generation)
);

video_overlay_filter #(.READ_LATENCY(2)) overlay_filter (
  .clk(dvi_clk),
  .variant(vga_overlay_variant),
  .luma_odd(overlay_sample_luma_odd),
  .luma_edge(overlay_sample_luma_edge),
  .luma_weight(overlay_sample_luma_weight),
  .word_odd(overlay_sample_word_odd),
  .word_edge(overlay_sample_word_edge),
  .chroma_weight(overlay_sample_chroma_weight),
  .row_weight(overlay_row_weight),
  .top_even(overlay_read_top_even),
  .top_odd(overlay_read_top_odd),
  .bottom_even(overlay_read_bottom_even),
  .bottom_odd(overlay_read_bottom_odd),
  .yuv422(overlay_yuv422)
);

/* The base pixel and the window decision belong to the cycle after the one
 * whose overlay_pos_x they show; hold them for the line-buffer path. */
reg [24*OVERLAY_FILTER_DELAY-1:0] overlay_base_delay = 0;
reg [OVERLAY_FILTER_DELAY-1:0] overlay_draw_delay = 0;
always @(posedge dvi_clk) begin
  overlay_base_delay <= {overlay_base_delay[24*OVERLAY_FILTER_DELAY-25:0],
                         pixout[23:0]};
  overlay_draw_delay <= {overlay_draw_delay[OVERLAY_FILTER_DELAY-2:0],
                         overlay_in_window && overlay_line_ready &&
                         vga_overlay_source_mode == 0};
end

wire [23:0] overlay_rgb;
wire overlay_pixel_active;
video_overlay_pixel overlay_pixel (
  .clk(dvi_clk),
  .resetn(aresetn),
  .base_rgb(overlay_base_delay[24*OVERLAY_FILTER_DELAY-1 -: 24]),
  .key_rgb(vga_overlay_key_rgb),
  .key_enable(vga_overlay_key_enable),
  .overlay_enable(overlay_draw_delay[OVERLAY_FILTER_DELAY-1]),
  .yuv422(overlay_yuv422),
  .variant(3'd0),
  .luma_phase(1'b0),
  .out_rgb(overlay_rgb),
  .out_overlay(overlay_pixel_active)
);

wire [31:0] pixout_composited = vga_overlay_enable
  ? {8'b0, overlay_rgb} : pixout;
wire [31:0] composed_rgb = vga_overlay_enable
  ? ((sprite_on_overlay && sprite_pix_overlay != 'hff00ff)
      ? sprite_pix_overlay : pixout_sl)
  : ((sprite_on && sprite_pix != 'hff00ff) ? sprite_pix : pixout_sl);
wire signed [16:0] viewport_output_x = viewport_output_x_position;
wire [11:0] viewport_output_y = viewport_output_y_row;
wire viewport_output_active = viewport_geometry_ready &&
  counter_y >= vga_scale_y_factor &&
  viewport_output_x >= $signed({1'b0, vga_viewport_x}) &&
  viewport_output_x < $signed({1'b0, viewport_output_x_end}) &&
  viewport_output_y >= vga_viewport_y &&
  viewport_output_y < vga_viewport_y + vga_viewport_height;
wire [1:0] scanline_content_y = counter_y_d2 - vga_viewport_y[1:0];

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
  vga_h_rez_plus_pipe <= vga_h_rez + PIPE_DELAY;
  vga_v_rez <= screen_height;
  vga_h_max <= screen_h_max - 1'b1;
  fractional_line_update_x <= vga_h_max - 1'b1;
  vga_v_max <= screen_v_max - 1'b1;
  /* screen_h_sync_start/end are quasi-static mode config written once per
   * mode change on m_axis_vid_aclk (OP_HS); this continuous resample on
   * dvi_clk is a snapshot crossing, not a 2-flop CDC - by design. */
  vga_h_sync_start <= screen_h_sync_start;
  vga_h_sync_end <= screen_h_sync_end;

  vga_h_sync_start_delayed <= vga_h_sync_start + PIPE_DELAY +
                              (vga_overlay_enable ? OVERLAY_PIPE_DELAY : 0);
  vga_h_sync_end_delayed <= vga_h_sync_end + PIPE_DELAY +
                            (vga_overlay_enable ? OVERLAY_PIPE_DELAY : 0);
  vga_h_rez_delayed <= vga_h_rez_plus_pipe +
                       (vga_overlay_enable ? OVERLAY_PIPE_DELAY : 0);

  vga_v_sync_start <= screen_v_sync_start;
  vga_v_sync_end <= screen_v_sync_end;
  vga_scale_x <= scale_x;
  vga_scale_y <= scale_y;
  vga_scale_source_rows <= scale_source_rows;
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
    vga_overlay_x_step <= overlay_x_step;
    vga_overlay_y_step <= overlay_y_step;
    vga_overlay_x_start <= overlay_x_start;
    vga_overlay_y_start <= overlay_y_start;
    overlay_pos_x_step <= overlay_x_step;
    overlay_pos_y_step <= overlay_y_step;
    overlay_pos_x_start <= overlay_x_start;
    overlay_pos_y_start <= overlay_y_start;
    vga_overlay_luma_max <= overlay_luma_max;
    vga_overlay_word_max <= overlay_word_max;
    vga_overlay_row_max <= overlay_row_max;
    vga_overlay_scale_epoch <= overlay_scale_epoch;
    vga_overlay_key_rgb <= overlay_key_rgb;
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
  /* These frame-stable viewport terms keep the wide rectangle bounds off
   * counter_scanout's reset path. */
  viewport_has_left_border <= vga_viewport_x != 0;
  viewport_scanout_first <= vga_viewport_x - 1'b1;
  viewport_scanout_last <= {1'b0, vga_viewport_x} +
                            {1'b0, vga_viewport_width};

  overlay_scheduler_y_in_window <=
    overlay_local_y >= 0 &&
    overlay_local_y < $signed({1'b0, vga_overlay_height});
  overlay_scheduler_screen_y_nonnegative <= overlay_screen_y >= 0;
  overlay_scheduler_row_visible <= counter_y < vga_v_rez;
  if (counter_x == 0) begin
    overlay_pos_x <= overlay_pos_x_start;
  end else if (overlay_screen_x >= overlay_visible_x0_minus_one &&
               overlay_screen_x < overlay_visible_x1_minus_one) begin
    overlay_pos_x <= overlay_pos_x + overlay_pos_x_step;
  end

  if (counter_x == 0 && counter_y == 0) begin
    overlay_pos_y <= overlay_pos_y_start;
  end else if (counter_x == vga_h_rez &&
               overlay_screen_y >= 0 &&
               overlay_local_y >= 0 &&
               overlay_local_y + 17'sd1 <
                 $signed({1'b0, vga_overlay_height})) begin
    overlay_pos_y <= overlay_pos_y + overlay_pos_y_step;
  end
  if (counter_y == 0) begin
    vga_sprite_x <= sprite_x;
    vga_sprite_y <= sprite_y;
  end
  vga_sprite_x2 <= vga_sprite_x+(SPRITE_W<<sprite_dbl);
  vga_sprite_y2 <= vga_sprite_y+(SPRITE_H<<sprite_dbl);
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
  if (((viewport_has_left_border &&
        counter_x < viewport_scanout_first) ||
       {1'b0, counter_x} > viewport_scanout_last) &&
      !(counter_x >= vga_h_max && !viewport_has_left_border)) begin
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

  sprite_pix_delay <= {sprite_pix_delay[24*OVERLAY_PIPE_DELAY-25:0],
                       sprite_pix};
  sprite_on_delay <= {sprite_on_delay[OVERLAY_PIPE_DELAY-2:0], sprite_on};

counter_y_d1 <= counter_y[1:0];
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

  if (source_sync_video_hidden)
    dvi_rgb <= 32'b0;
  else
    dvi_rgb <= viewport_output_active ? composed_rgb : 32'b0;

  /* The refill side can overwrite the bank from the completed row during
   * horizontal blanking. Select the upcoming row one clock before raster
   * wrap so that stale-bank data has cleared the registered pixel pipeline
   * before active column zero. */
  if (counter_x + 1'b1 == vga_h_max)
    scanout_source_line_row <= next_scanout_source_line_latched;

  /* These registered coordinates advance on the same edge as counter_x/y.
   * Their values therefore retain the existing pixel/row phase while
   * breaking counter arithmetic away from overlay BRAM/tag/control logic. */
  if (counter_x >= vga_h_max) begin
    overlay_screen_x_position <= overlay_screen_x_origin;
    overlay_local_x_position <= overlay_local_x_origin;
    {overlay_displayed_line_row, overlay_displayed_next_row,
     overlay_row_weight} <= frame_wrap_this_line
      ? overlay_row_start_sample : overlay_row_next_sample;
    viewport_output_x_position <= -$signed(PIPE_DELAY) -
      (vga_overlay_enable ? $signed(OVERLAY_PIPE_DELAY) : 17'sd0);
    viewport_output_y_row <= next_raster_y - vga_scale_y_factor;

    if (frame_wrap_this_line) begin
      overlay_screen_y_row <= overlay_screen_y_origin;
      overlay_local_y_row <= overlay_local_y_origin;
    end else begin
      overlay_screen_y_row <= overlay_screen_y_row + 17'sd1;
      overlay_local_y_row <= overlay_local_y_row + 17'sd1;
    end
  end else begin
    overlay_screen_x_position <= overlay_screen_x_position + 17'sd1;
    overlay_local_x_position <= overlay_local_x_position + 17'sd1;
    viewport_output_x_position <= viewport_output_x_position + 17'sd1;
  end

  if (counter_x >= vga_h_max) begin
    counter_x <= 0;
    if (frame_wrap_this_line) begin
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

  if (counter_x==vga_h_rez) begin
    if (!fractional_scale_y) begin
      need_line_fetch <= need_line_fetch_row_valid
        ? need_line_fetch_candidate : 12'b0;
    end else if ({1'b0, counter_y} < fractional_content_start) begin
      need_line_fetch <= 0;
    end else if (fractional_prefetch_valid_latched) begin
      /* Once source row N is on screen its opposite bank is free. Start
       * filling N+1 during the first duplicate of N, not its last. */
      need_line_fetch <= fractional_prefetch_source_line_latched;
    end else begin
      /* Reset the request label in bottom blanking before frame resync.
       * Holding the final row here leaves the restarted MM2S stream parked
       * behind the prior frame's last label. */
      need_line_fetch <= 0;
    end
  end

  /* Fetching runs ahead of the display: the fetch stepper issues the next
   * distinct source row whenever the line fetcher has a free bank, and skips
   * rows the display has already passed so a late row never leaves the
   * fetcher behind for the rest of the frame. */
  if (!vga_overlay_enable) begin
    overlay_fetch_line <= 0;
    overlay_fetch_active <= 0;
    overlay_consumed_valid <= 0;
  end else if (overlay_accepted_generation !=
               vga_overlay_frame_generation ||
               overlay_applied_scale_epoch != vga_overlay_scale_epoch) begin
    /* Firmware advances the generation at vblank with the VDMA stopped. The
     * line-buffer's atomic acknowledgement proves its AXI domain has
     * stopped treating the old frame as active; only then may the pixel
     * domain request the first visible source line. Geometry can also change
     * while a simulation or legacy producer retains the same generation:
     * restart at the correctly clipped source row either way. */
    vga_overlay_frame_generation <= overlay_accepted_generation;
    overlay_applied_scale_epoch <= vga_overlay_scale_epoch;
    overlay_fetch_row <= overlay_fetch_first_row;
    overlay_fetch_issued <= 0;
    overlay_fetch_active <= 1;
    overlay_frame_fetch_done <= 0;
    overlay_consumed_valid <= 0;
  end else begin
    if (overlay_scheduler_y_in_window &&
        overlay_scheduler_screen_y_nonnegative &&
        overlay_scheduler_row_visible) begin
      overlay_consumed_line <= overlay_scheduler_line;
      overlay_consumed_valid <= 1;
    end
    if (counter_y == vga_v_rez && counter_x == 0)
      overlay_frame_fetch_done <= 1;

    if (overlay_fetch_active &&
        (overlay_fetch_issued || overlay_fetch_passed) &&
        overlay_fetch_row < vga_overlay_row_max) begin
      overlay_fetch_row <= overlay_fetch_row + 1'b1;
      overlay_fetch_issued <= 0;
    end else if (overlay_fetch_active && !overlay_fetch_issued &&
                 !overlay_fetch_passed && !overlay_frame_fetch_done &&
                 overlay_fetch_ready) begin
      overlay_fetch_line <= overlay_fetch_row;
      overlay_fetch_request <= 1;
      overlay_fetch_issued <= 1;
    end
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
  if (counter_y >= vga_v_rez && !frame_wrap_this_line) begin
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
