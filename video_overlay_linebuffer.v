`timescale 1ns / 1ps
/*
 * P96 packed-YUV422 overlay line fetcher.
 *
 * The overlay VDMA is deliberately paced one source line at a time. BRAM
 * banks (BANK_BITS sets how many) hold the two source rows the bilinear
 * filter is blending plus later ones, so the fetcher can run ahead of
 * scanout. A downscale blends more than one new source row per raster line;
 * running ahead spreads those rows over several raster lines instead of
 * needing two whole rows inside one line. Bank ownership is explicit: the
 * pixel domain picks a bank that is empty or holds a row above the top row
 * on display (consumed_line), and the fetcher drains skipped producer rows
 * while tracking their real sequential row numbers. Only the VDMA
 * frame-start marker starts row counting: a stream that was stopped and
 * restarted is picked up at its SOF by the next generation, never
 * displayed at the wrong vertical position.
 *
 * Every row is written to two copies, each split into even and odd
 * macropixel words, so one pixel clock reads macropixels k and k+1 of both
 * the top and the bottom row.
 *
 * Exactly one request is outstanding at a time. Each request carries a
 * toggle that its completion echoes, so a completion left over from before a
 * generation change cannot retire a newer request.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
module video_overlay_linebuffer #(
  parameter MAX_PIXELS = 2560,
  parameter ADDR_WIDTH = 11,
  parameter BANK_BITS = 2
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
  output        fetch_ready,
  input  [11:0] consumed_line,
  input         consumed_valid,

  input                   pixel_clk,
  input                   display_enable,
  /* Word addresses within a row, split by parity: a bilinear sample needs
   * the two adjacent macropixels k and k+1, which are always one even and
   * one odd word. Drive them from registers; the words follow two pixel
   * clocks later. */
  input  [ADDR_WIDTH-2:0] read_addr_even,
  input  [ADDR_WIDTH-2:0] read_addr_odd,
  /* The two source rows a bilinear output row blends (equal at the bottom
   * edge and for unscaled rows). */
  input  [11:0]           displayed_line,
  input  [11:0]           displayed_line_next,
  output [31:0]           read_top_even,
  output [31:0]           read_top_odd,
  output [31:0]           read_bottom_even,
  output [31:0]           read_bottom_odd,
  output                  displayed_line_ready,
  output reg [11:0]       scheduler_line = 0,
  output reg [31:0]       accepted_generation = 0
);

localparam BANKS = 1 << BANK_BITS;
localparam MAX_MACROPIXELS = (MAX_PIXELS + 1) / 2;
/* Each row is stored twice (one copy per bilinear row port) and split into
 * even and odd words, so four memories each hold half of every bank. Bank
 * selection is the MSBs of the BRAM address, so each bank begins on a
 * 2**(ADDR_WIDTH-1) boundary. Allocate the complete address space; sizing
 * this to the used words leaves the upper banks outside MEMORY_SIZE on
 * hardware even though behavioral XPM simulation may appear to retain those
 * words. */
localparam MEMORY_WORDS = (1 << (ADDR_WIDTH - 1 + BANK_BITS));
localparam MSG_WIDTH = 12 + BANK_BITS + 1;
/* Completions carry the low bits of the generation that produced them. A
 * stale completion is at most one handshake old, and the generation
 * advances at most once per handshake, so eight bits cannot alias. */
localparam GEN_TAG_BITS = 8;
localparam COMPLETION_WIDTH = MSG_WIDTH + GEN_TAG_BITS;
localparam ST_SEEK_FRAME = 2'd0;
localparam ST_READ_LINE  = 2'd1;
localparam ST_WAIT_LINE  = 2'd2;

reg [1:0] state = ST_SEEK_FRAME;
reg [11:0] target_line = 0;
reg [BANK_BITS-1:0] target_bank = 0;
reg target_toggle = 0;
reg [11:0] stream_line = 0;
reg [11:0] loaded_line = 12'hfff;
reg [ADDR_WIDTH-1:0] write_addr = 0;
reg [31:0] loaded_generation = 0;
reg [11:0] requested_line_axis = 0;
reg [BANK_BITS-1:0] requested_bank_axis = 0;
reg requested_toggle_axis = 0;
reg requested_line_pending = 0;
reg await_zero_request = 0;

/* A binary row number can change several bits on one increment. Sampling
 * that bus with independent two-flop chains can produce a row number that
 * never existed. Use XPM bundled-data handshakes in both directions so the
 * payload is held stable while a single synchronized request crosses. */
reg [MSG_WIDTH-1:0] fetch_request_data = 0;
reg fetch_request_pending = 0;
reg fetch_request_send = 0;
wire fetch_request_received;
wire [MSG_WIDTH-1:0] fetch_request_axis;
wire [11:0] fetch_line_axis = fetch_request_axis[11:0];
wire [BANK_BITS-1:0] fetch_bank_axis = fetch_request_axis[12 +: BANK_BITS];
wire fetch_toggle_axis = fetch_request_axis[MSG_WIDTH-1];
wire fetch_line_axis_valid;

xpm_cdc_handshake #(
  .DEST_EXT_HSK(0),
  .DEST_SYNC_FF(3),
  .INIT_SYNC_FF(0),
  .SIM_ASSERT_CHK(0),
  .SRC_SYNC_FF(3),
  .WIDTH(MSG_WIDTH)
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

reg [COMPLETION_WIDTH-1:0] completed_line_data = 0;
reg completed_line_pending = 0;
reg completed_line_send = 0;
wire completed_line_received;
wire [COMPLETION_WIDTH-1:0] completed_line_pixel_data;
wire [11:0] completed_line_pixel = completed_line_pixel_data[11:0];
wire [BANK_BITS-1:0] completed_bank_pixel =
  completed_line_pixel_data[12 +: BANK_BITS];
wire completed_toggle_pixel = completed_line_pixel_data[MSG_WIDTH-1];
wire [GEN_TAG_BITS-1:0] completed_generation_pixel =
  completed_line_pixel_data[MSG_WIDTH +: GEN_TAG_BITS];
wire completed_line_pixel_valid;

xpm_cdc_handshake #(
  .DEST_EXT_HSK(0),
  .DEST_SYNC_FF(3),
  .INIT_SYNC_FF(0),
  .SIM_ASSERT_CHK(0),
  .SRC_SYNC_FF(3),
  .WIDTH(COMPLETION_WIDTH)
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
/* The value in flight: src_in must stay stable for the whole handshake, and
 * generation_sent must record what the pixel side actually received, even
 * if firmware commits another generation meanwhile (that one is sent by
 * the next handshake). */
reg [31:0] generation_send_value = 0;
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
  .src_in(generation_send_value),
  .src_send(generation_send),
  .src_rcv(generation_received),
  .dest_clk(pixel_clk),
  .dest_out(accepted_generation_pixel),
  .dest_req(accepted_generation_pixel_valid),
  .dest_ack(1'b0)
);

wire accept = s_axis_tvalid && s_axis_tready;
wire storing_seek_zero = state == ST_SEEK_FRAME && s_axis_tuser &&
                         target_line == 0;
wire storing_stream_line = state == ST_READ_LINE &&
                           stream_line == target_line;
wire stream_line_completed = accept && s_axis_tlast &&
                             (storing_seek_zero || storing_stream_line);
wire [11:0] completed_line_value = storing_seek_zero ? 12'b0 : stream_line;
/* A repeated request for the row already loaded is answered from the bank
 * that holds it; the stream has moved past that row. */
wire relabel_loaded_line = state == ST_WAIT_LINE && requested_line_pending &&
                           requested_line_axis == loaded_line;
wire write_in_range = write_addr < MAX_MACROPIXELS;
wire [3:0] write_enable = (accept && storing_stream_line && write_in_range)
                            ? s_axis_tkeep : 4'b0000;

/* Do not let a newly restarted VDMA complete line zero before the pixel
 * domain has consumed the generation acknowledgement and invalidated the
 * prior frame's ready tags.
 *
 * A frame start that arrives while a later row is still being read is the
 * VDMA restarted at vblank, with the next generation about to follow (the
 * firmware restarts the channel, then commits the generation). Hold it
 * instead of consuming it under the old generation: the new generation's
 * SOF search then takes this same beat. Consuming it would leave the new
 * generation waiting a whole stream frame for the next SOF. */
assign s_axis_tready = fetch_enable &&
                       fetch_generation == generation_sent &&
                       !await_zero_request &&
                       (state == ST_SEEK_FRAME ||
                        (state == ST_READ_LINE && !s_axis_tuser));

always @(posedge axis_clk) begin
  if (!axis_resetn) begin
    state <= ST_SEEK_FRAME;
    target_line <= 0;
    target_bank <= 0;
    target_toggle <= 0;
    stream_line <= 0;
    loaded_line <= 12'hfff;
    write_addr <= 0;
    loaded_generation <= 0;
    requested_line_axis <= 0;
    requested_bank_axis <= 0;
    requested_toggle_axis <= 0;
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
        target_toggle <= fetch_toggle_axis;
        stream_line <= 0;
        write_addr <= 0;
        await_zero_request <= 0;
      end else begin
        requested_line_axis <= fetch_line_axis;
        requested_bank_axis <= fetch_bank_axis;
        requested_toggle_axis <= fetch_toggle_axis;
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
        /* tready is low for an SOF beat here, so every accepted beat
         * belongs to the current stream frame. */
        if (accept) begin
          if (stream_line == target_line)
            write_addr <= write_addr + 1'b1;

          if (s_axis_tlast) begin
            write_addr <= 0;
            stream_line <= stream_line + 1'b1;
            if (stream_line == target_line) begin
              loaded_line <= stream_line;
              state <= ST_WAIT_LINE;
            end
          end
        end
      end

      default: begin /* ST_WAIT_LINE */
        if (requested_line_pending) begin
          requested_line_pending <= 0;
          /* A relabelled row keeps its bank; only the toggle retires. */
          target_toggle <= requested_toggle_axis;
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
    if (stream_line_completed) begin
      completed_line_data <= {loaded_generation[GEN_TAG_BITS-1:0],
                              target_toggle, target_bank,
                              completed_line_value};
      completed_line_pending <= 1;
    end else if (relabel_loaded_line && fetch_enable &&
                 fetch_generation == loaded_generation) begin
      completed_line_data <= {loaded_generation[GEN_TAG_BITS-1:0],
                              requested_toggle_axis, target_bank,
                              loaded_line};
      completed_line_pending <= 1;
    end
  end
end

/* A generation is acknowledged only through the same atomic CDC primitive.
 * It changes at most once per vblank, far slower than this handshake. */
always @(posedge axis_clk) begin
  if (!axis_resetn) begin
    generation_send <= 0;
    generation_send_value <= 0;
    generation_sent <= 0;
  end else if (generation_send) begin
    if (generation_received) begin
      generation_send <= 0;
      generation_sent <= generation_send_value;
    end
  end else if (!generation_received && fetch_enable &&
               fetch_generation != generation_sent) begin
    generation_send <= 1;
    generation_send_value <= fetch_generation;
  end
end

/* The first SOF beat is accepted in ST_SEEK_FRAME rather than ST_READ_LINE,
 * so include that one write explicitly. */
wire [3:0] memory_we = (accept && storing_seek_zero)
                       ? s_axis_tkeep : write_enable;
wire [ADDR_WIDTH-1:0] write_word =
                       state == ST_SEEK_FRAME ? {ADDR_WIDTH{1'b0}} : write_addr;
wire [ADDR_WIDTH+BANK_BITS-2:0] memory_write_addr =
                       {target_bank, write_word[ADDR_WIDTH-1:1]};

/* Pixel-domain ready tags are updated only from atomic completion messages.
 * A fetch request invalidates its destination bank before the AXI domain can
 * begin overwriting it. */
reg [BANKS-1:0] pixel_ready_valid = 0;
reg [11:0] pixel_ready_line [0:BANKS-1];
reg request_inflight = 0;
reg [BANK_BITS-1:0] request_bank = 0;
reg request_toggle = 0;
reg [BANKS-1:0] bank_free = 0;

wire [BANKS-1:0] top_match;
wire [BANKS-1:0] bottom_match;
reg [BANK_BITS-1:0] top_bank;
reg [BANK_BITS-1:0] bottom_bank;
reg [BANK_BITS-1:0] free_bank;
integer b;

genvar g;
generate
  for (g = 0; g < BANKS; g = g + 1) begin : bank_tags
    initial pixel_ready_line[g] = 12'hfff;
    assign top_match[g] = pixel_ready_valid[g] &&
                          pixel_ready_line[g] == displayed_line;
    assign bottom_match[g] = pixel_ready_valid[g] &&
                             pixel_ready_line[g] == displayed_line_next;
  end
endgenerate

always @* begin
  top_bank = 0;
  for (b = BANKS - 1; b >= 0; b = b - 1)
    if (top_match[b])
      top_bank = b;
  bottom_bank = 0;
  for (b = BANKS - 1; b >= 0; b = b - 1)
    if (bottom_match[b])
      bottom_bank = b;
  free_bank = 0;
  for (b = BANKS - 1; b >= 0; b = b - 1)
    if (bank_free[b])
      free_bank = b;
end

/* The read addresses arrive registered; the bank selection is registered
 * here too, so the BRAM address pins see only routing. It follows the row
 * tags one pixel clock late, which only matters while a late row lands
 * mid-line. */
reg [BANK_BITS-1:0] top_bank_q = 0;
reg [BANK_BITS-1:0] bottom_bank_q = 0;
always @(posedge pixel_clk) begin
  top_bank_q <= top_bank;
  bottom_bank_q <= bottom_bank;
end

/* Memory m: bit 1 selects the row copy (0 top, 1 bottom), bit 0 the word
 * parity. Every beat is written to both copies of its parity. Reads use the
 * BRAM output register (READ_LATENCY 2) for the 150 MHz pixel clock. */
wire [31:0] read_data [0:3];
generate
  for (g = 0; g < 4; g = g + 1) begin : line_memory
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
      .ADDR_WIDTH_A(ADDR_WIDTH - 1 + BANK_BITS),
      .RST_MODE_A("SYNC"),
      .READ_DATA_WIDTH_B(32),
      .ADDR_WIDTH_B(ADDR_WIDTH - 1 + BANK_BITS),
      .READ_RESET_VALUE_B("0"),
      .READ_LATENCY_B(2),
      .WRITE_MODE_B("read_first"),
      .RST_MODE_B("SYNC")
    ) memory (
      .sleep(1'b0),
      .clka(axis_clk),
      .ena(1'b1),
      .wea(write_word[0] == (g % 2) ? memory_we : 4'b0000),
      .addra(memory_write_addr),
      .dina(s_axis_tdata),
      .injectsbiterra(1'b0),
      .injectdbiterra(1'b0),
      .clkb(pixel_clk),
      .rstb(1'b0),
      .enb(1'b1),
      .regceb(1'b1),
      .addrb({(g / 2) ? bottom_bank_q : top_bank_q,
              (g % 2) ? read_addr_odd : read_addr_even}),
      .doutb(read_data[g]),
      .sbiterrb(),
      .dbiterrb()
    );
  end
endgenerate

assign read_top_even = read_data[0];
assign read_top_odd = read_data[1];
assign read_bottom_even = read_data[2];
assign read_bottom_odd = read_data[3];
assign displayed_line_ready = display_enable && |top_match && |bottom_match;
assign fetch_ready = display_enable && !request_inflight && |bank_free;
/* The generation the pixel side serves, including one acknowledged in this
 * very cycle. */
wire [GEN_TAG_BITS-1:0] current_generation_tag = accepted_generation_pixel_valid
  ? accepted_generation_pixel[GEN_TAG_BITS-1:0]
  : accepted_generation[GEN_TAG_BITS-1:0];

always @(posedge pixel_clk) begin
  /* Registered snapshot of the top row on display, for the fetch
   * scheduler's consumed-row tracking. */
  scheduler_line <= displayed_line;

  /* A bank is free when it is empty or holds a row the display has passed.
   * The outstanding request's bank stays reserved until its completion
   * arrives, so this registered view can never hand out a bank whose
   * completion landed in the same cycle. */
  for (b = 0; b < BANKS; b = b + 1)
    bank_free[b] <= !(request_inflight && request_bank == b) &&
                    (!pixel_ready_valid[b] ||
                     (consumed_valid && pixel_ready_line[b] < consumed_line));

  if (fetch_request_send) begin
    if (fetch_request_received)
      fetch_request_send <= 0;
  end else if (!fetch_request_received && fetch_request_pending) begin
    fetch_request_send <= 1;
    fetch_request_pending <= 0;
  end

  if (accepted_generation_pixel_valid) begin
    accepted_generation <= accepted_generation_pixel;
    pixel_ready_valid <= 0;
    request_inflight <= 0;
  end

  /* Keep this after invalidation: completed data in a bank is valid. A
   * completion from the previous generation can cross after (or with) the
   * generation acknowledgement; its bank may already be reassigned, so it
   * is dropped. New-generation rows cannot complete before the
   * acknowledgement, because the stream is held until the first request,
   * which the pixel side only makes after it. */
  if (completed_line_pixel_valid &&
      completed_generation_pixel == current_generation_tag) begin
    pixel_ready_line[completed_bank_pixel] <= completed_line_pixel;
    pixel_ready_valid[completed_bank_pixel] <= 1;
    if (completed_toggle_pixel == request_toggle)
      request_inflight <= 0;
  end

  /* A request reserves and invalidates its bank last: the fetcher is about
   * to overwrite it, whatever an older completion said. */
  if (fetch_request) begin
    fetch_request_data <= {~request_toggle, free_bank, fetch_line};
    fetch_request_pending <= 1;
    request_toggle <= ~request_toggle;
    request_inflight <= 1;
    request_bank <= free_bank;
    pixel_ready_valid[free_bank] <= 0;
  end
end

endmodule
