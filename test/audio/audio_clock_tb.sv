`timescale 1ns / 1ps

module audio_clock_tb;
  localparam [15:0] TDM_LEFT = 16'h2468;
  localparam [15:0] TDM_RIGHT = 16'hace1;
  localparam real ADAU_DATA_DELAY_NS = 40.0;

  reg bclk_in = 1'b0;
  reg fclk_in = 1'b0;
  reg lrclk_in = 1'b1;
  reg resetn = 1'b0;
  reg sdata_in = 1'b0;

  wire bclk_out;
  wire mclk_out;
  wire rx_lrclk_out;
  wire rx_sdata_out;

  integer source_frames = 0;
  integer output_edges = 0;
  reg observed_output_lrclk = 1'b0;

  audio_clock dut (
    .bclk_in(bclk_in),
    .fclk_in(fclk_in),
    .lrclk_in(lrclk_in),
    .resetn(resetn),
    .sdata_in(sdata_in),
    .bclk_out(bclk_out),
    .mclk_out(mclk_out),
    .rx_lrclk_out(rx_lrclk_out),
    .rx_sdata_out(rx_sdata_out)
  );

  always #5 fclk_in = ~fclk_in;
  always #40.690104 bclk_in = ~bclk_in;

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

  task automatic drive_frame_body(input reg [255:0] value);
    integer bit_index;
    begin
      for (bit_index = 0; bit_index < 127;
           bit_index = bit_index + 1) begin
        @(negedge bclk_in);
        #(ADAU_DATA_DELAY_NS);
        sdata_in = value[255 - bit_index];
      end

      @(negedge bclk_in);
      lrclk_in = 1'b1;
      #(ADAU_DATA_DELAY_NS);
      sdata_in = value[128];

      for (bit_index = 128; bit_index < 255;
           bit_index = bit_index + 1) begin
        @(negedge bclk_in);
        #(ADAU_DATA_DELAY_NS);
        sdata_in = value[255 - bit_index];
      end

      @(negedge bclk_in);
      lrclk_in = 1'b0;
      #(ADAU_DATA_DELAY_NS);
      sdata_in = value[0];
      source_frames = source_frames + 1;
    end
  endtask

  initial begin : codec_source
    reg [255:0] frame;
    wait (resetn);
    repeat (12) @(negedge bclk_in);
    @(negedge bclk_in);
    lrclk_in = 1'b0;
    #(ADAU_DATA_DELAY_NS);
    sdata_in = 1'b0;

    frame = tdm_frame(TDM_LEFT, TDM_RIGHT);
    forever
      drive_frame_body(frame);
  end

  always @(posedge bclk_out) begin
    if (rx_lrclk_out != observed_output_lrclk) begin
      observed_output_lrclk = rx_lrclk_out;
      output_edges = output_edges + 1;
    end
  end

  initial begin
    repeat (8) @(posedge fclk_in);
    resetn = 1'b1;

    while (source_frames < 6)
      @(posedge fclk_in);

    if (dut.normalized_left !== TDM_LEFT)
      $fatal(1, "left slot mismatch: %04x", dut.normalized_left);
    if (dut.normalized_right !== TDM_RIGHT)
      $fatal(1, "right slot mismatch: %04x", dut.normalized_right);
    if (output_edges < 4)
      $fatal(1, "normalized I2S frame clock did not advance");
    if (mclk_out !== bclk_in)
      $fatal(1, "codec master clock is not source-synchronous");

    $display(
        "audio_clock fixed TDM8 slot-0/1 bridge PASS (L=%04x R=%04x edges=%0d)",
        dut.normalized_left, dut.normalized_right, output_edges);
    $finish;
  end

  initial begin
    #30000000;
    $fatal(1, "timeout");
  end
endmodule
