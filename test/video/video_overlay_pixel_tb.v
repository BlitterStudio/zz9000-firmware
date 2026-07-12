`timescale 1ns / 1ps
module video_overlay_pixel_tb;
  reg clk = 0;
  reg resetn = 0;
  reg [23:0] base_rgb = 24'h123456;
  reg [23:0] key_rgb = 24'h123456;
  reg key_enable = 0;
  reg overlay_enable = 0;
  reg [31:0] yuv422 = 0;
  reg [2:0] variant = 0;
  reg luma_phase = 0;
  wire [23:0] out_rgb;
  wire out_overlay;
  integer failures = 0;

  video_overlay_pixel dut(
    .clk(clk), .resetn(resetn), .base_rgb(base_rgb), .key_rgb(key_rgb),
    .key_enable(key_enable), .overlay_enable(overlay_enable),
    .yuv422(yuv422), .variant(variant), .luma_phase(luma_phase),
    .out_rgb(out_rgb), .out_overlay(out_overlay)
  );
  always #5 clk = ~clk;

  task settle;
    begin
      repeat (4) @(posedge clk);
      #1;
    end
  endtask

  task check_output;
    input [23:0] rgb;
    input active;
    begin
      if (out_rgb !== rgb || out_overlay !== active) begin
        $display("FAIL got rgb=%h active=%b expected=%h/%b",
                 out_rgb, out_overlay, rgb, active);
        failures = failures + 1;
      end
    end
  endtask

  initial begin
    repeat (2) @(posedge clk);
    resetn = 1;

    overlay_enable = 0;
    settle; check_output(24'h123456, 0);

    /* Studio-range black and white, CGX Y0 U Y1 V. */
    overlay_enable = 1;
    yuv422 = {8'd128, 8'd235, 8'd128, 8'd16};
    luma_phase = 0;
    settle; check_output(24'h000000, 1);
    luma_phase = 1;
    settle; check_output(24'hffffff, 1);

    /* Color key reject then accept. */
    key_enable = 1;
    key_rgb = 24'h654321;
    settle; check_output(24'h123456, 0);
    key_rgb = 24'h123456;
    settle; check_output(24'hffffff, 1);

    /* Same red sample in all five P96 packed layouts. */
    key_enable = 0;
    luma_phase = 0;
    variant = 0; yuv422 = {8'd240, 8'd81, 8'd90, 8'd81};
    settle; check_output(24'hff0000, 1);
    variant = 1; yuv422 = {8'd90, 8'd81, 8'd240, 8'd81};
    settle; check_output(24'hff0000, 1);
    variant = 2; yuv422 = {8'd81, 8'd240, 8'd81, 8'd90};
    settle; check_output(24'hff0000, 1);
    variant = 3; yuv422 = {8'd240, 8'd90, 8'd81, 8'd81};
    settle; check_output(24'hff0000, 1);
    variant = 4; yuv422 = {8'd81, 8'd81, 8'd90, 8'd240};
    settle; check_output(24'hff0000, 1);

    if (failures == 0) $display("video_overlay_pixel_tb: PASS");
    else $display("video_overlay_pixel_tb: %0d failure(s)", failures);
    $finish(failures != 0);
  end
endmodule
