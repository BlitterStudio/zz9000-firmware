`timescale 1ns / 1ps

/*
 * Exercise the production Xilinx I2S transmitter with the ADAU1701-facing
 * clocks. The serial-slot trace is intentionally keyed to the LRCLK seen by
 * the transmitter so head and reference builds can be compared without
 * relying on implementation hierarchy.
 */
module audio_clock_i2s_tx_tb;
  reg raw_bclk = 1'b0;
  reg raw_lrclk = 1'b0;
  reg bridge_resetn = 1'b0;
  reg fclk_in = 1'b0;
  reg axi_clk = 1'b0;
  reg axi_resetn = 1'b0;
  reg axi_awvalid = 1'b0;
  wire axi_awready;
  reg [7:0] axi_awaddr = 8'b0;
  reg axi_wvalid = 1'b0;
  wire axi_wready;
  reg [31:0] axi_wdata = 32'b0;
  wire axi_bvalid;
  reg axi_bready = 1'b0;
  wire [1:0] axi_bresp;
  reg axi_arvalid = 1'b0;
  wire axi_arready;
  reg [7:0] axi_araddr = 8'b0;
  wire axi_rvalid;
  reg axi_rready = 1'b0;
  wire [31:0] axi_rdata;
  wire [1:0] axi_rresp;
  wire tx_irq;
  wire tx_ready;
  reg [31:0] tx_data = 32'ha55a_3cc3;
  reg [2:0] tx_tid = 3'b0;
  reg tx_valid = 1'b0;

  wire divided_bclk;
  wire bridge_mclk;
`ifdef REGISTERED_TX_LRCLK
  wire tx_lrclk;
`else
  wire tx_lrclk = raw_lrclk;
`endif
  wire tx_sdata;
  integer raw_edges = 0;
  integer emitted_slots = 0;
  reg sclk_capture_pending = 1'b0;
  integer accepted_words = 0;
  integer serial_bits = 0;
  reg observed_lrclk = 1'b0;
  reg [15:0] serial_word = 16'b0;

  audio_clock bridge (
    .bclk_in(raw_bclk),
    .fclk_in(fclk_in),
    .lrclk_in(raw_lrclk),
    .resetn(bridge_resetn),
    .sdata_in(1'b0),
    .bclk_out(divided_bclk),
    .mclk_out(bridge_mclk),
`ifdef REGISTERED_TX_LRCLK
    .tx_lrclk_out(tx_lrclk),
`endif
    .rx_lrclk_out(),
    .rx_sdata_out()
  );

  zz9000_ps_i2s_transmitter_0_0 transmitter (
    .s_axi_ctrl_aclk(axi_clk),
    .s_axi_ctrl_aresetn(axi_resetn),
    .aud_mclk(bridge_mclk),
    .aud_mrst(1'b0),
    .s_axis_aud_aclk(axi_clk),
    .s_axis_aud_aresetn(axi_resetn),
    .s_axi_ctrl_awvalid(axi_awvalid),
    .s_axi_ctrl_awready(axi_awready),
    .s_axi_ctrl_awaddr(axi_awaddr),
    .s_axi_ctrl_wvalid(axi_wvalid),
    .s_axi_ctrl_wready(axi_wready),
    .s_axi_ctrl_wdata(axi_wdata),
    .s_axi_ctrl_bvalid(axi_bvalid),
    .s_axi_ctrl_bready(axi_bready),
    .s_axi_ctrl_bresp(axi_bresp),
    .s_axi_ctrl_arvalid(axi_arvalid),
    .s_axi_ctrl_arready(axi_arready),
    .s_axi_ctrl_araddr(axi_araddr),
    .s_axi_ctrl_rvalid(axi_rvalid),
    .s_axi_ctrl_rready(axi_rready),
    .s_axi_ctrl_rdata(axi_rdata),
    .s_axi_ctrl_rresp(axi_rresp),
    .irq(tx_irq),
    .lrclk_in(tx_lrclk),
    .sclk_in(divided_bclk),
    .sdata_0_out(tx_sdata),
    .s_axis_aud_tdata(tx_data),
    .s_axis_aud_tid(tx_tid),
    .s_axis_aud_tvalid(tx_valid),
    .s_axis_aud_tready(tx_ready)
  );

  always #5 fclk_in = ~fclk_in;
  always #20 axi_clk = ~axi_clk;
  always #40.690104 raw_bclk = ~raw_bclk;

  /* ADAU1701 TDM8 LRCLK is low for slots 0..3 and high for slots 4..7. */
  always @(negedge raw_bclk) begin
    raw_edges = raw_edges + 1;
    if (raw_edges == 128) begin
      raw_lrclk = ~raw_lrclk;
      raw_edges = 0;
    end
  end

  task automatic axi_write(input reg [7:0] address, input reg [31:0] data);
    reg address_done;
    reg data_done;
    begin
      @(negedge axi_clk);
      axi_awaddr = address;
      axi_awvalid = 1'b1;
      axi_wdata = data;
      axi_wvalid = 1'b1;
      axi_bready = 1'b1;
      address_done = 1'b0;
      data_done = 1'b0;
      while (!address_done || !data_done) begin
        @(posedge axi_clk);
        if (axi_awvalid && axi_awready)
          address_done = 1'b1;
        if (axi_wvalid && axi_wready)
          data_done = 1'b1;
        @(negedge axi_clk);
        if (address_done)
          axi_awvalid = 1'b0;
        if (data_done)
          axi_wvalid = 1'b0;
      end
      while (!axi_bvalid)
        @(posedge axi_clk);
      if (axi_bresp != 2'b00)
        $fatal(1, "AXI write response %02b", axi_bresp);
      @(negedge axi_clk);
      axi_bready = 1'b0;
    end
  endtask

  always @(posedge axi_clk) begin
    if (tx_valid && tx_ready) begin
      accepted_words = accepted_words + 1;
      tx_data <= {tx_data[30:0], tx_data[31]};
    end
  end

  always @(posedge divided_bclk)
    sclk_capture_pending <= 1'b1;

  always @(posedge raw_bclk) begin
    reg [15:0] completed_word;
    #1;
    if (sclk_capture_pending && !bridge.reset_active) begin
      sclk_capture_pending <= 1'b0;
      if (tx_lrclk != observed_lrclk) begin
        observed_lrclk <= tx_lrclk;
        serial_bits <= 1;
        serial_word <= {15'b0, tx_sdata};
      end else if (serial_bits < 16) begin
        completed_word = {serial_word[14:0], tx_sdata};
        serial_word <= completed_word;
        serial_bits <= serial_bits + 1;
        if (serial_bits == 15) begin
          $display("TX_SLOT lrclk=%0b raw_phase=%0d bits=%04x",
                   observed_lrclk, raw_edges, completed_word);
          emitted_slots = emitted_slots + 1;
        end
      end
    end
  end

  initial begin
    repeat (8) @(posedge fclk_in);
    bridge_resetn = 1'b1;
    repeat (8) @(posedge axi_clk);
    axi_resetn = 1'b1;
    repeat (8) @(posedge axi_clk);
    axi_write(8'h08, 32'h00000001);
    tx_valid = 1'b1;

    while (emitted_slots < 32)
      @(posedge divided_bclk);

    if (accepted_words == 0)
      $fatal(1, "transmitter did not accept AXIS audio");
    $display("audio_clock + Xilinx I2S transmitter alignment PASS slots=%0d words=%0d",
             emitted_slots, accepted_words);
    $finish;
  end

  initial begin
    #1000000;
    $fatal(1, "timeout");
  end
endmodule
