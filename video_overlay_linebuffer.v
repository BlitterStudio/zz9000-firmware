`timescale 1ns / 1ps
/*
 * P96 packed-YUV422 overlay line fetcher.
 *
 * The overlay VDMA is deliberately paced one source line at a time.  Two
 * BRAM banks let scanout consume line N while MM2S fills line N+1.  The VDMA
 * frame-start marker is used whenever the requested line wraps to zero, so a
 * stopped/restarted or overrun stream recovers at the next frame instead of
 * displaying source lines at the wrong vertical position.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
module video_overlay_linebuffer #(
  parameter MAX_PIXELS = 2560,
  parameter ADDR_WIDTH = 11
)(
  input         axis_clk,
  input         axis_resetn,
  input  [31:0] s_axis_tdata,
  input  [3:0]  s_axis_tkeep,
  input         s_axis_tlast,
  input         s_axis_tuser,
  input         s_axis_tvalid,
  output        s_axis_tready,

  input         fetch_enable,
  input  [31:0] fetch_generation,
  input  [11:0] requested_line,

  input                   pixel_clk,
  input  [ADDR_WIDTH-1:0] read_addr,
  input                   read_bank,
  output [31:0]           read_data,
  output                  requested_line_ready
);

localparam MAX_MACROPIXELS = (MAX_PIXELS + 1) / 2;
/* Bank selection is the MSB of the BRAM address, so each bank begins on a
 * 2**ADDR_WIDTH boundary. Allocate the complete address space; sizing this
 * as only 2*MAX_MACROPIXELS leaves bank 1 outside MEMORY_SIZE on hardware
 * even though behavioral XPM simulation may appear to retain those words. */
localparam MEMORY_WORDS = (1 << (ADDR_WIDTH + 1));
localparam ST_SEEK_FRAME = 2'd0;
localparam ST_READ_LINE  = 2'd1;
localparam ST_WAIT_LINE  = 2'd2;

reg [1:0] state = ST_SEEK_FRAME;
reg [11:0] target_line = 0;
reg [11:0] loaded_line = 12'hfff;
reg [ADDR_WIDTH-1:0] write_addr = 0;
reg ready_valid0 = 0;
reg ready_valid1 = 0;
reg [11:0] ready_line0 = 12'hfff;
reg [11:0] ready_line1 = 12'hfff;

reg [11:0] request_sync1 = 0;
reg [11:0] request_sync2 = 0;
reg enable_sync1 = 0;
reg enable_sync2 = 0;
reg [31:0] generation_sync1 = 0;
reg [31:0] generation_sync2 = 0;
reg [31:0] loaded_generation = 0;

wire accept = s_axis_tvalid && s_axis_tready;
wire unexpected_sof = accept && state == ST_READ_LINE && s_axis_tuser &&
                      target_line != 0;
wire write_in_range = write_addr < MAX_MACROPIXELS;
wire [3:0] write_enable = (accept && state == ST_READ_LINE && write_in_range)
                            ? s_axis_tkeep : 4'b0000;
wire [ADDR_WIDTH:0] memory_write_addr = {target_line[0], write_addr};
wire [ADDR_WIDTH:0] memory_read_addr = {read_bank, read_addr};

assign s_axis_tready = enable_sync2 &&
                       (state == ST_SEEK_FRAME || state == ST_READ_LINE);

always @(posedge axis_clk) begin
  request_sync1 <= requested_line;
  request_sync2 <= request_sync1;
  enable_sync1 <= fetch_enable;
  enable_sync2 <= enable_sync1;
  generation_sync1 <= fetch_generation;
  generation_sync2 <= generation_sync1;

  if (!axis_resetn || !enable_sync2) begin
    state <= ST_SEEK_FRAME;
    target_line <= 0;
    loaded_line <= 12'hfff;
    write_addr <= 0;
    ready_valid0 <= 0;
    ready_valid1 <= 0;
    loaded_generation <= generation_sync2;
  end else if (generation_sync2 != loaded_generation) begin
    /* A vblank buffer flip invalidates any prefetched line from the old
     * surface and re-arms the SOF search on the new VDMA frame store. */
    state <= ST_SEEK_FRAME;
    target_line <= 0;
    loaded_line <= 12'hfff;
    write_addr <= 0;
    ready_valid0 <= 0;
    ready_valid1 <= 0;
    loaded_generation <= generation_sync2;
  end else begin
    case (state)
      ST_SEEK_FRAME: begin
        /* Drain to SOF without writing.  The SOF beat is line zero. */
        if (accept && s_axis_tuser) begin
          target_line <= 0;
          write_addr <= 1;
          if (s_axis_tkeep != 0) begin
            /* The RAM write port uses target_line=0 in this state. */
            ready_valid0 <= 0;
          end
          if (s_axis_tlast) begin
            ready_line0 <= 0;
            ready_valid0 <= 1;
            loaded_line <= 0;
            state <= ST_WAIT_LINE;
          end else begin
            state <= ST_READ_LINE;
          end
        end
      end

      ST_READ_LINE: begin
        if (accept) begin
          /* An unexpected SOF means the producer wrapped while recovery was
           * pending.  Re-label this beat as line zero and recover there. */
          if (s_axis_tuser && target_line != 0) begin
            target_line <= 0;
            write_addr <= 1;
            ready_valid0 <= 0;
          end else begin
            write_addr <= write_addr + 1'b1;
          end

          if (s_axis_tlast) begin
            if (s_axis_tuser && target_line != 0) begin
              ready_line0 <= 0;
              ready_valid0 <= 1;
              loaded_line <= 0;
            end else if (target_line[0]) begin
              ready_line1 <= target_line;
              ready_valid1 <= 1;
              loaded_line <= target_line;
            end else begin
              ready_line0 <= target_line;
              ready_valid0 <= 1;
              loaded_line <= target_line;
            end
            state <= ST_WAIT_LINE;
          end
        end
      end

      default: begin /* ST_WAIT_LINE */
        if (request_sync2 != loaded_line) begin
          target_line <= request_sync2;
          write_addr <= 0;
          if (request_sync2 == 0) begin
            state <= ST_SEEK_FRAME;
          end else begin
            if (request_sync2[0])
              ready_valid1 <= 0;
            else
              ready_valid0 <= 0;
            state <= ST_READ_LINE;
          end
        end
      end
    endcase
  end
end

/* The first SOF beat is accepted in ST_SEEK_FRAME rather than ST_READ_LINE,
 * so include that one write explicitly. */
wire [3:0] memory_we = (accept && state == ST_SEEK_FRAME && s_axis_tuser)
                       ? s_axis_tkeep : write_enable;
wire [ADDR_WIDTH:0] effective_write_addr =
                       (state == ST_SEEK_FRAME || unexpected_sof)
                         ? {(ADDR_WIDTH+1){1'b0}} : memory_write_addr;

xpm_memory_sdpram #(
  .MEMORY_SIZE(MEMORY_WORDS * 32),
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
  .WRITE_DATA_WIDTH_A(32),
  .BYTE_WRITE_WIDTH_A(8),
  .ADDR_WIDTH_A(ADDR_WIDTH + 1),
  .RST_MODE_A("SYNC"),
  .READ_DATA_WIDTH_B(32),
  .ADDR_WIDTH_B(ADDR_WIDTH + 1),
  .READ_RESET_VALUE_B("0"),
  .READ_LATENCY_B(1),
  .WRITE_MODE_B("read_first"),
  .RST_MODE_B("SYNC")
) overlay_line_memory (
  .sleep(1'b0),
  .clka(axis_clk),
  .ena(1'b1),
  .wea(memory_we),
  .addra(effective_write_addr),
  .dina(s_axis_tdata),
  .injectsbiterra(1'b0),
  .injectdbiterra(1'b0),
  .clkb(pixel_clk),
  .rstb(1'b0),
  .enb(1'b1),
  .regceb(1'b1),
  .addrb(memory_read_addr),
  .doutb(read_data),
  .sbiterrb(),
  .dbiterrb()
);

/* Ready metadata is written well before scanout reaches the bank.  Two
 * sampling stages keep the tag stable in the pixel domain. */
reg valid0_sync1 = 0, valid0_sync2 = 0;
reg valid1_sync1 = 0, valid1_sync2 = 0;
reg fetch_enable_sync1 = 0, fetch_enable_sync2 = 0;
reg [11:0] line0_sync1 = 12'hfff, line0_sync2 = 12'hfff;
reg [11:0] line1_sync1 = 12'hfff, line1_sync2 = 12'hfff;
always @(posedge pixel_clk) begin
  fetch_enable_sync1 <= fetch_enable;
  fetch_enable_sync2 <= fetch_enable_sync1;
  valid0_sync1 <= ready_valid0;
  valid0_sync2 <= valid0_sync1;
  valid1_sync1 <= ready_valid1;
  valid1_sync2 <= valid1_sync1;
  line0_sync1 <= ready_line0;
  line0_sync2 <= line0_sync1;
  line1_sync1 <= ready_line1;
  line1_sync2 <= line1_sync1;
end

assign requested_line_ready = fetch_enable_sync2 && (read_bank
  ? (valid1_sync2 && line1_sync2 == requested_line)
  : (valid0_sync2 && line0_sync2 == requested_line));

endmodule
