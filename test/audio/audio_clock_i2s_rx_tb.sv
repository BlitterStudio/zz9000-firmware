`timescale 1ns / 1ps

/*
 * Integration test for the fixed TDM8 bridge and the exact encrypted Xilinx
 * i2s_receiver_v1_0_2 used by the Vivado 2018.3 generated design.
 */
module audio_clock_i2s_rx_tb;
  localparam [15:0] TDM_LEFT = 16'h2468;
  localparam [15:0] TDM_RIGHT = 16'hace1;
  localparam real ADAU_DATA_DELAY_NS = 40.0;

  reg fclk_in = 1'b0;
  reg raw_bclk = 1'b0;
  reg raw_lrclk = 1'b1;
  reg raw_sdata = 1'b0;
  reg bridge_resetn = 1'b0;

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

  wire divided_bclk;
  wire bridge_mclk;
  wire receiver_lrclk;
  wire receiver_sdata;
  wire receiver_irq;
  wire [31:0] axis_tdata;
  wire [2:0] axis_tid;
  wire axis_tvalid;
  reg axis_tready = 1'b1;

  integer axis_words = 0;
  integer checked_left = 0;
  integer checked_right = 0;

  audio_clock bridge (
    .bclk_in(raw_bclk),
    .fclk_in(fclk_in),
    .lrclk_in(raw_lrclk),
    .resetn(bridge_resetn),
    .sdata_in(raw_sdata),
    .bclk_out(divided_bclk),
    .mclk_out(bridge_mclk),
    .rx_lrclk_out(receiver_lrclk),
    .rx_sdata_out(receiver_sdata)
  );

  zz9000_ps_i2s_receiver_0_0 receiver (
    .s_axi_ctrl_aclk(axi_clk),
    .s_axi_ctrl_aresetn(axi_resetn),
    .aud_mclk(bridge_mclk),
    .aud_mrst(1'b0),
    .m_axis_aud_aclk(axi_clk),
    .m_axis_aud_aresetn(axi_resetn),
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
    .irq(receiver_irq),
    .lrclk_in(receiver_lrclk),
    .sclk_in(divided_bclk),
    .sdata_0_in(receiver_sdata),
    .m_axis_aud_tdata(axis_tdata),
    .m_axis_aud_tid(axis_tid),
    .m_axis_aud_tvalid(axis_tvalid),
    .m_axis_aud_tready(axis_tready)
  );

  always #5 fclk_in = ~fclk_in;
  always #20 axi_clk = ~axi_clk;
  always #40.690104 raw_bclk = ~raw_bclk;

  function automatic [255:0] tdm_frame(
    input reg [15:0] slot0,
    input reg [15:0] slot1
  );
    reg [255:0] value;
    begin
      value = 256'b0;
      value[255 -: 16] = slot0;
      value[223 -: 16] = slot1;
      value[191 -: 16] = 16'h1111;
      value[159 -: 16] = 16'h2222;
      value[127 -: 16] = 16'h3333;
      value[95 -: 16] = 16'h4444;
      value[63 -: 16] = 16'h5555;
      value[31 -: 16] = 16'h6666;
      tdm_frame = value;
    end
  endfunction

  task automatic axi_write(
    input reg [7:0] address,
    input reg [31:0] data
  );
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

  task automatic drive_frame_body(input reg [255:0] value);
    integer bit_index;
    begin
      for (bit_index = 0; bit_index < 127;
           bit_index = bit_index + 1) begin
        @(negedge raw_bclk);
        #(ADAU_DATA_DELAY_NS);
        raw_sdata = value[255 - bit_index];
      end

      @(negedge raw_bclk);
      raw_lrclk = 1'b1;
      #(ADAU_DATA_DELAY_NS);
      raw_sdata = value[128];

      for (bit_index = 128; bit_index < 255;
           bit_index = bit_index + 1) begin
        @(negedge raw_bclk);
        #(ADAU_DATA_DELAY_NS);
        raw_sdata = value[255 - bit_index];
      end

      @(negedge raw_bclk);
      raw_lrclk = 1'b0;
      #(ADAU_DATA_DELAY_NS);
      raw_sdata = value[0];
    end
  endtask

  initial begin : codec_source
    reg [255:0] frame;
    wait (bridge_resetn);
    repeat (12) @(negedge raw_bclk);
    @(negedge raw_bclk);
    raw_lrclk = 1'b0;
    #(ADAU_DATA_DELAY_NS);
    raw_sdata = 1'b0;

    frame = tdm_frame(TDM_LEFT, TDM_RIGHT);
    forever
      drive_frame_body(frame);
  end

  always @(posedge axi_clk) begin
    if (axis_tvalid && axis_tready) begin
      axis_words = axis_words + 1;
      if (axis_tid == 3'd0 && axis_tdata[27:12] == TDM_LEFT)
        checked_left = checked_left + 1;
      if (axis_tid == 3'd1 && axis_tdata[27:12] == TDM_RIGHT)
        checked_right = checked_right + 1;
    end
  end

  initial begin
    repeat (8) @(posedge fclk_in);
    bridge_resetn = 1'b1;

    repeat (8) @(posedge axi_clk);
    axi_resetn = 1'b1;
    repeat (8) @(posedge axi_clk);
    axi_write(8'h08, 32'h00000001);

    while (checked_left < 6 || checked_right < 6)
      @(posedge axi_clk);

    if (axis_words < 12)
      $fatal(1, "only %0d AXIS words received", axis_words);

    $display(
        "audio_clock + Xilinx I2S receiver fixed TDM PASS (L=%0d R=%0d)",
        checked_left, checked_right);
    $finish;
  end

  initial begin
    #30000000;
    $fatal(1, "timeout");
  end
endmodule
