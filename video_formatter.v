`timescale 1ns / 1ps
/*
 * MNT ZZ9000 Amiga Graphics and Coprocessor Card Firmware
 * Video Stream Formatter
 *
 * Copyright (C) 2019-2020, Lukas F. Hartmann <lukas@mntre.com>
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
  input [31:0] m_axis_vid_tdata,
  input m_axis_vid_tlast,
  output m_axis_vid_tready,
  input [0:0]  m_axis_vid_tuser,
  input m_axis_vid_tvalid,
  input m_axis_vid_aclk,
  input aresetn,
  
  input dvi_clk,
  output reg dvi_hsync,
  output reg dvi_vsync,
  output reg dvi_active_video,
  output reg [31:0] dvi_rgb,
  
  // control inputs for setting palette, width/height, scaling
  input [31:0] control_data,
  input [7:0] control_op,
  input control_interlace,
  output reg [1:0]control_vblank
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

localparam CMODE_8BIT=0;
localparam CMODE_16BIT=1;
localparam CMODE_32BIT=2;
localparam CMODE_15BIT=4;

reg [11:0] screen_width; // = 720;
reg [11:0] screen_height; // = 576;
reg scale_x = 0;
reg scale_y = 1; // amiga boots in 640x256, so double the resolution vertically
reg [31:0] palette[255:0];
reg [2:0] colormode = CMODE_32BIT;
reg vsync_request;
reg sync_polarity = 1; // negative polarity

reg [15:0] screen_h_max; //= 864;
reg [15:0] screen_v_max; //= 625;
reg [15:0] screen_h_sync_start; //= 732;
reg [15:0] screen_h_sync_end; //= 796;
reg [15:0] screen_v_sync_start; //= 581;
reg [15:0] screen_v_sync_end; //= 586;

localparam MAXWIDTH=1280;
reg [31:0] line_buffer[MAXWIDTH-1:0];

// (input) vdma state
reg [3:0] next_input_state;
reg [11:0] inptr;
reg ready_for_vdma;

assign m_axis_vid_tready = ready_for_vdma;

reg [11:0] counter_x; // vga domain
reg [11:0] counter_y; // vga domain
reg [11:0] need_line_fetch; // vga domain

reg [11:0] need_line_fetch_reg;
reg [11:0] need_line_fetch_reg2;
reg [11:0] need_line_fetch_reg3;
reg [11:0] last_line_fetch;

wire [31:0] pixin = m_axis_vid_tdata;
wire pixin_valid = m_axis_vid_tvalid;
wire pixin_end_of_line = m_axis_vid_tlast;
wire pixin_framestart = m_axis_vid_tuser[0];

reg scale_y_effective;

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
reg [11:0] vga_report_y; // vga domain
reg [11:0] vga_report_y_next; // vga domain

always @(posedge m_axis_vid_aclk)
  begin
    if (~aresetn) begin
      ready_for_vdma <= 0;
      next_input_state <= 0;
      inptr <= 0;
    end
    
    need_frame_sync_reg <= need_frame_sync;
    need_line_fetch_reg  <= need_line_fetch; // sync to clock domain
    need_line_fetch_reg2 <= need_line_fetch_reg>>scale_y_effective; // line duplication
    
    scale_y_effective <= control_interlace ? 0 : scale_y;
    
    if (pixin_valid && ready_for_vdma) begin
      line_buffer[inptr] <= pixin;
      // disabling this makes the picture go wild
      if (pixin_framestart) // we might have missed the frame start
        inptr <= 1;
      else if (pixin_end_of_line) // next after this is the first pixel of the line (0)
        inptr <= 0;
      else
        inptr <= inptr + 1'b1;
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
          
          // line_fetch_reg2 == 0
          if (need_frame_sync_reg==1) begin
            next_input_state <= 4'h2;
          end
        end
    endcase
  end

reg [31:0] control_data_in;
reg [7:0] control_op_in;
reg control_interlace_in;
reg [31:0] control_data_in2;
reg [7:0] control_op_in2;
reg control_interlace_in2;

// control input
always @(posedge m_axis_vid_aclk)
begin
  control_op_in        <= control_op;
  control_data_in      <= control_data;
  control_interlace_in <= control_interlace;
  
  if (next_input_state==0) begin
    vsync_request <= 0;
  end
  
  if (control_interlace_in != control_interlace) begin
    vsync_request <= 1;
  end
  
  case (control_op_in)
    OP_PALETTE: palette[control_data_in[31:24]] <= control_data_in[23:0];
    OP_DIMENSIONS: begin
        screen_height <= control_data_in[31:16];
        screen_width  <= control_data_in[15:0];
      end
    OP_SCALE: begin
        scale_x  <= control_data_in[0];
        scale_y  <= control_data_in[1];
        sprite_dbl <= control_data_in[1];
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
        sync_polarity <= 1;
        screen_h_max <= 864;
        screen_v_max <= 625;
        screen_h_sync_start <= 732;
        screen_h_sync_end <= 796;
        screen_v_sync_start <= 581;
        screen_v_sync_end <= 586;
        scale_x <= 0;
        scale_y <= 1;
        screen_width <= 720;
        screen_height <= 576;
        colormode <= CMODE_32BIT;
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
  endcase
end

reg [31:0] palout;
reg [11:0] vga_v_rez;
reg [11:0] vga_h_rez;
reg [11:0] vga_v_max;
reg [11:0] vga_h_max;
reg [11:0] vga_h_sync_start;
reg [11:0] vga_h_sync_end;
reg [11:0] vga_v_sync_start;
reg [11:0] vga_v_sync_end;
reg [11:0] counter_scanout;
reg [2:0] vga_colormode;

reg [11:0] vga_h_rez_shifted;
reg [11:0] vga_v_rez_shifted;

reg vga_scale_x = 0;
reg vga_scale_y = 0;
reg [31:0] pixout;
reg [7:0]  pixout8;
reg [15:0] pixout16;
reg [31:0] pixout32;
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

always @(posedge dvi_clk) begin
  vga_h_rez <= screen_width;
  vga_v_rez <= screen_height;
  vga_h_max <= screen_h_max;
  vga_v_max <= screen_v_max;
  vga_h_sync_start <= screen_h_sync_start; //  + 4
  vga_h_sync_end <= screen_h_sync_end; //  + 4
  vga_v_sync_start <= screen_v_sync_start;
  vga_v_sync_end <= screen_v_sync_end;
  vga_scale_x <= scale_x;
  vga_scale_y <= scale_y;
  vga_colormode <= colormode;
  vga_sync_polarity <= sync_polarity;
  if (counter_y == 0) begin
    vga_sprite_x <= sprite_x;
    vga_sprite_y <= sprite_y;
  end
  vga_sprite_x2 <= vga_sprite_x+(SPRITE_W<<sprite_dbl);
  vga_sprite_y2 <= vga_sprite_y+(SPRITE_H<<sprite_dbl);
  vga_sprite_dbl <= sprite_dbl;
  vga_report_y_next <= report_y;
  
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
    //4'b0100: counter_scanout_step <= 1; // 15 bit
    //4'b1100: counter_scanout_step <= 3;
  endcase
  
  if (counter_x>vga_h_rez) begin
    counter_scanout  <= 0;
    counter_subpixel <= counter_scanout_step;
  end else begin
    if (counter_subpixel == 0) begin
      counter_subpixel <= counter_scanout_step;
      counter_scanout  <= counter_scanout + 1'b1;
    end else
      counter_subpixel <= counter_subpixel - 1'b1;
  end
  
  pixout32 <= line_buffer[counter_scanout];
  
  if (vga_colormode==CMODE_16BIT)
    // 16 bit 5r6g5b
    pixout32_dly <= {8'b0,blue16,green16,red16};
  //else if (vga_colormode==CMODE_15BIT)
  //  // 15 bit 5r5g5b for shapeshifter
  //  pixout32_dly <= {8'b0,blue15,green15,red15};
  else
    pixout32_dly <= pixout32;
  pixout32_dly2 <= pixout32_dly;
  
  palout <= palette[pixout8];
  
  case (vga_colormode)
    CMODE_8BIT:  pixout <= palout;
    CMODE_16BIT: pixout <= pixout32_dly;
    CMODE_32BIT: pixout <= pixout32_dly2;
  endcase
  
  sprite_pix <= sprite_buffer[((sprite_py>>sprite_dbl)<<5)+(sprite_px>>sprite_dbl)];
  if (counter_y >= vga_sprite_y && counter_y < vga_sprite_y2 
      && counter_x >= vga_sprite_x && counter_x < vga_sprite_x2) begin
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
  
  dvi_rgb <= (sprite_on && sprite_pix!='hff00ff) ? sprite_pix : pixout;
  
  if (counter_x > vga_h_max) begin
    counter_x <= 0;
    if (counter_y > vga_v_max) begin
      counter_y <= 0;
      sprite_px <= 0;
      sprite_py <= 0;
    end else begin
      counter_y <= counter_y + 1'b1;
    end
  end else begin
    counter_x <= counter_x + 1'b1;
  end
  
  if (counter_x==vga_h_rez) begin
    if (counter_y<vga_v_rez-1'b1)
      need_line_fetch <= counter_y + 1'b1;
    else
      need_line_fetch <= 0;
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
  
  if (counter_x >= vga_h_sync_start && counter_x < vga_h_sync_end) begin
    dvi_hsync <= 1^vga_sync_polarity;
  end else
    dvi_hsync <= 0^vga_sync_polarity;
    
  if (counter_y >= vga_v_sync_start && counter_y < vga_v_sync_end) begin
    dvi_vsync <= 1^vga_sync_polarity;
  end else begin
    dvi_vsync <= 0^vga_sync_polarity;
  end
  
  // 4 clocks pipeline delay
  vga_h_rez_shifted <= vga_h_rez+4;
  
  // account for 1 line of vdma wrap-around
  if (counter_y>vga_scale_y && counter_y<=(vga_v_rez + vga_scale_y) && counter_x==4)
    dvi_active_video <= 1;
    
  if (counter_x==vga_h_rez_shifted)
    dvi_active_video <= 0;
end

endmodule
