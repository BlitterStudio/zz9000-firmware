`timescale 1ns / 1ps
/*
 * P96 packed-YUV422 overlay line fetcher.
 *
 * The overlay VDMA is deliberately paced one source line at a time.  Two
 * BRAM banks let scanout consume one source line while MM2S fills the next
 * requested line. Bank ownership is explicit rather than derived from source
 * row parity, so downscaling can skip an even number of rows without
 * overwriting the displayed bank. The fetcher drains skipped producer rows
 * while tracking their real sequential row numbers. The VDMA frame-start
 * marker recovers a stopped/restarted or overrun stream at the next frame
 * instead of displaying source lines at the wrong vertical position.
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
  input  [11:0] fetch_line,
  input         fetch_request,

  input                   pixel_clk,
  input                   display_enable,
  input  [ADDR_WIDTH-1:0] read_addr,
  input  [11:0]           displayed_line,
  output [31:0]           read_data,
  output                  displayed_line_ready,
  output reg [31:0]       accepted_generation = 0
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
reg target_bank = 0;
reg [11:0] stream_line = 0;
reg [11:0] loaded_line = 12'hfff;
reg [ADDR_WIDTH-1:0] write_addr = 0;
reg [31:0] loaded_generation = 0;
reg [11:0] requested_line_axis = 0;
reg requested_bank_axis = 0;
reg requested_line_pending = 0;
reg await_zero_request = 0;

/* A binary row number can change several bits on one increment. Sampling
 * that bus with independent two-flop chains can produce a row number that
 * never existed. Use XPM bundled-data handshakes in both directions so the
 * payload is held stable while a single synchronized request crosses. */
reg [12:0] fetch_request_data = 0;
reg fetch_request_pending = 0;
reg fetch_request_send = 0;
wire fetch_request_received;
wire [12:0] fetch_request_axis;
wire [11:0] fetch_line_axis = fetch_request_axis[11:0];
wire fetch_bank_axis = fetch_request_axis[12];
wire fetch_line_axis_valid;

xpm_cdc_handshake #(
  .DEST_EXT_HSK(0),
  .DEST_SYNC_FF(3),
  .INIT_SYNC_FF(0),
  .SIM_ASSERT_CHK(0),
  .SRC_SYNC_FF(3),
  .WIDTH(13)
) fetch_line_cdc (
  .src_clk(pixel_clk),
  .src_in(fetch_request_data),
  .src_send(fetch_request_send),
  .src_rcv(fetch_request_received),
  .dest_clk(axis_clk),
  .dest_out(fetch_request_axis),
  .dest_req(fetch_line_axis_valid),
  .dest_ack(1'b0)
);

reg [12:0] completed_line_data = 0;
reg completed_line_pending = 0;
reg completed_line_send = 0;
wire completed_line_received;
wire [12:0] completed_line_pixel_data;
wire [11:0] completed_line_pixel = completed_line_pixel_data[11:0];
wire completed_bank_pixel = completed_line_pixel_data[12];
wire completed_line_pixel_valid;

xpm_cdc_handshake #(
  .DEST_EXT_HSK(0),
  .DEST_SYNC_FF(3),
  .INIT_SYNC_FF(0),
  .SIM_ASSERT_CHK(0),
  .SRC_SYNC_FF(3),
  .WIDTH(13)
) completed_line_cdc (
  .src_clk(axis_clk),
  .src_in(completed_line_data),
  .src_send(completed_line_send),
  .src_rcv(completed_line_received),
  .dest_clk(pixel_clk),
  .dest_out(completed_line_pixel_data),
  .dest_req(completed_line_pixel_valid),
  .dest_ack(1'b0)
);

reg generation_send = 0;
reg [31:0] generation_sent = 0;
wire generation_received;
wire [31:0] accepted_generation_pixel;
wire accepted_generation_pixel_valid;

xpm_cdc_handshake #(
  .DEST_EXT_HSK(0),
  .DEST_SYNC_FF(3),
  .INIT_SYNC_FF(0),
  .SIM_ASSERT_CHK(0),
  .SRC_SYNC_FF(3),
  .WIDTH(32)
) generation_cdc (
  .src_clk(axis_clk),
  .src_in(fetch_generation),
  .src_send(generation_send),
  .src_rcv(generation_received),
  .dest_clk(pixel_clk),
  .dest_out(accepted_generation_pixel),
  .dest_req(accepted_generation_pixel_valid),
  .dest_ack(1'b0)
);

wire accept = s_axis_tvalid && s_axis_tready;
wire unexpected_sof = accept && state == ST_READ_LINE && s_axis_tuser;
wire storing_seek_zero = state == ST_SEEK_FRAME && s_axis_tuser &&
                         target_line == 0;
wire storing_stream_line = state == ST_READ_LINE &&
                           (unexpected_sof || stream_line == target_line);
wire completed_line_event = accept && s_axis_tlast &&
                            (storing_seek_zero || storing_stream_line);
wire [11:0] completed_line_value =
  (storing_seek_zero || unexpected_sof) ? 12'b0 : stream_line;
wire write_in_range = write_addr < MAX_MACROPIXELS;
wire [3:0] write_enable = (accept && storing_stream_line && write_in_range)
                            ? s_axis_tkeep : 4'b0000;
wire [ADDR_WIDTH:0] memory_write_addr = {target_bank, write_addr};
wire [ADDR_WIDTH:0] memory_read_addr;

/* Do not let a newly restarted VDMA complete line zero before the pixel
 * domain has consumed the generation acknowledgement and invalidated the
 * prior frame's ready tags. */
assign s_axis_tready = fetch_enable &&
                       fetch_generation == generation_sent &&
                       !await_zero_request &&
                       (state == ST_SEEK_FRAME || state == ST_READ_LINE);

always @(posedge axis_clk) begin
  if (!axis_resetn) begin
    state <= ST_SEEK_FRAME;
    target_line <= 0;
    target_bank <= 0;
    stream_line <= 0;
    loaded_line <= 12'hfff;
    write_addr <= 0;
    loaded_generation <= 0;
    requested_line_axis <= 0;
    requested_bank_axis <= 0;
    requested_line_pending <= 0;
    await_zero_request <= 0;
  end else if (!fetch_enable) begin
    state <= ST_SEEK_FRAME;
    target_line <= 0;
    target_bank <= 0;
    stream_line <= 0;
    loaded_line <= 12'hfff;
    write_addr <= 0;
    requested_line_axis <= 0;
    requested_bank_axis <= 0;
    requested_line_pending <= 0;
    await_zero_request <= 0;
  end else if (fetch_generation != loaded_generation) begin
    /* A vblank buffer flip invalidates any prefetched line from the old
     * surface and re-arms the SOF search on the new VDMA frame store. */
    state <= ST_SEEK_FRAME;
    target_line <= 0;
    target_bank <= 0;
    stream_line <= 0;
    loaded_line <= 12'hfff;
    write_addr <= 0;
    loaded_generation <= fetch_generation;
    requested_line_pending <= 0;
    await_zero_request <= 1;
  end else begin
    if (fetch_line_axis_valid) begin
      if (await_zero_request) begin
        /* The first request is also the pixel-domain proof that old ready
         * tags were invalidated. A clipped overlay may begin below source
         * row zero, so remember the requested target while seeking SOF. */
        target_line <= fetch_line_axis;
        target_bank <= fetch_bank_axis;
        stream_line <= 0;
        write_addr <= 0;
        await_zero_request <= 0;
      end else begin
        requested_line_axis <= fetch_line_axis;
        requested_bank_axis <= fetch_bank_axis;
        requested_line_pending <= 1;
      end
    end

    case (state)
      ST_SEEK_FRAME: begin
        /* Drain to SOF. Store line zero only when it is the requested row;
         * otherwise continue draining producer rows until target_line. */
        if (accept && s_axis_tuser) begin
          stream_line <= 0;
          write_addr <= target_line == 0 ? 1 : 0;
          if (s_axis_tlast && target_line == 0) begin
            loaded_line <= 0;
            state <= ST_WAIT_LINE;
            stream_line <= 1;
          end else begin
            state <= ST_READ_LINE;
            if (s_axis_tlast) begin
              stream_line <= 1;
              write_addr <= 0;
            end
          end
        end
      end

      ST_READ_LINE: begin
        if (accept) begin
          /* An unexpected SOF means the producer wrapped while recovery was
           * pending. Preserve the established recovery contract: line zero
           * becomes the completed row instead of being relabelled as the
           * stale requested row. */
          if (s_axis_tuser) begin
            target_line <= 0;
            stream_line <= 0;
            write_addr <= 1;
          end else if (stream_line == target_line) begin
            write_addr <= write_addr + 1'b1;
          end

          if (s_axis_tlast) begin
            write_addr <= 0;
            if (s_axis_tuser) begin
              loaded_line <= 0;
              stream_line <= 1;
              state <= ST_WAIT_LINE;
            end else if (stream_line == target_line) begin
              loaded_line <= stream_line;
              stream_line <= stream_line + 1'b1;
              state <= ST_WAIT_LINE;
            end else begin
              stream_line <= stream_line + 1'b1;
            end
          end
        end
      end

      default: begin /* ST_WAIT_LINE */
        if (requested_line_pending) begin
          requested_line_pending <= 0;
          if (requested_line_axis != loaded_line) begin
            target_line <= requested_line_axis;
            target_bank <= requested_bank_axis;
            write_addr <= 0;
            if (requested_line_axis < stream_line)
              state <= ST_SEEK_FRAME;
            else
              state <= ST_READ_LINE;
          end
        end
      end
    endcase
  end
end

/* Completed-line metadata is held until the pixel domain has accepted it.
 * The next line cannot finish before this round trip completes because its
 * fetch request originates from that same pixel-domain completion. */
always @(posedge axis_clk) begin
  if (!axis_resetn) begin
    completed_line_data <= 0;
    completed_line_pending <= 0;
    completed_line_send <= 0;
  end else begin
    if (completed_line_send) begin
      if (completed_line_received)
        completed_line_send <= 0;
    end else if (!completed_line_received && completed_line_pending) begin
      completed_line_send <= 1;
      completed_line_pending <= 0;
    end
    if (completed_line_event) begin
      completed_line_data <= {target_bank, completed_line_value};
      completed_line_pending <= 1;
    end
  end
end

/* A generation is acknowledged only through the same atomic CDC primitive.
 * It changes at most once per vblank, far slower than this handshake. */
always @(posedge axis_clk) begin
  if (!axis_resetn) begin
    generation_send <= 0;
    generation_sent <= 0;
  end else if (generation_send) begin
    if (generation_received) begin
      generation_send <= 0;
      generation_sent <= fetch_generation;
    end
  end else if (!generation_received && fetch_enable &&
               fetch_generation != generation_sent) begin
    generation_send <= 1;
  end
end

/* The first SOF beat is accepted in ST_SEEK_FRAME rather than ST_READ_LINE,
 * so include that one write explicitly. */
wire [3:0] memory_we = (accept && storing_seek_zero)
                       ? s_axis_tkeep : write_enable;
wire [ADDR_WIDTH:0] effective_write_addr =
                       (state == ST_SEEK_FRAME || unexpected_sof)
                         ? {target_bank, {ADDR_WIDTH{1'b0}}}
                         : memory_write_addr;

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

/* Pixel-domain ready tags are updated only from atomic completion messages.
 * A fetch request invalidates its destination bank before the AXI domain can
 * begin overwriting it. */
reg pixel_ready_valid0 = 0;
reg pixel_ready_valid1 = 0;
reg [11:0] pixel_ready_line0 = 12'hfff;
reg [11:0] pixel_ready_line1 = 12'hfff;
wire displayed_in_bank0 = pixel_ready_valid0 &&
                          pixel_ready_line0 == displayed_line;
wire displayed_in_bank1 = pixel_ready_valid1 &&
                          pixel_ready_line1 == displayed_line;
wire displayed_bank = displayed_in_bank1 && !displayed_in_bank0;
wire fetch_request_bank = displayed_in_bank0 ? 1'b1 :
                          displayed_in_bank1 ? 1'b0 : 1'b0;
assign memory_read_addr = {displayed_bank, read_addr};
always @(posedge pixel_clk) begin
  if (fetch_request) begin
    fetch_request_data <= {fetch_request_bank, fetch_line};
    fetch_request_pending <= 1;
    if (fetch_request_bank)
      pixel_ready_valid1 <= 0;
    else
      pixel_ready_valid0 <= 0;
  end

  if (fetch_request_send) begin
    if (fetch_request_received)
      fetch_request_send <= 0;
  end else if (!fetch_request_received && fetch_request_pending) begin
    fetch_request_send <= 1;
    fetch_request_pending <= 0;
  end

  if (accepted_generation_pixel_valid) begin
    accepted_generation <= accepted_generation_pixel;
    pixel_ready_valid0 <= 0;
    pixel_ready_valid1 <= 0;
  end

  /* Keep this after invalidation: if completion and a request coincide for
   * the same bank, completed data is valid and must win. */
  if (completed_line_pixel_valid) begin
    if (completed_bank_pixel) begin
      pixel_ready_line1 <= completed_line_pixel;
      pixel_ready_valid1 <= 1;
    end else begin
      pixel_ready_line0 <= completed_line_pixel;
      pixel_ready_valid0 <= 1;
    end
  end
end

assign displayed_line_ready = display_enable &&
  (displayed_line[0]
    ? ((pixel_ready_valid1 && pixel_ready_line1 == displayed_line) ||
       (pixel_ready_valid0 && pixel_ready_line0 == displayed_line))
    : ((pixel_ready_valid0 && pixel_ready_line0 == displayed_line) ||
       (pixel_ready_valid1 && pixel_ready_line1 == displayed_line)));

endmodule
