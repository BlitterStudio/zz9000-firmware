`timescale 1ns / 1ps

module vga_output_stage_tb;
reg pixel_clk = 1'b0;
reg [7:0] rgb_r_in = 8'h00;
reg [7:0] rgb_g_in = 8'h00;
reg [7:0] rgb_b_in = 8'h00;
reg hsync_in = 1'b0;
reg vsync_in = 1'b0;
reg de_in = 1'b0;
wire [7:0] VGA_R;
wire [7:0] VGA_G;
wire [7:0] VGA_B;
wire VGA_HS;
wire VGA_VS;
wire VGA_DE;
wire VGA_PCLK;
integer errors = 0;

vga_output_stage dut (
  .pixel_clk(pixel_clk),
  .rgb_r_in(rgb_r_in),
  .rgb_g_in(rgb_g_in),
  .rgb_b_in(rgb_b_in),
  .hsync_in(hsync_in),
  .vsync_in(vsync_in),
  .de_in(de_in),
  .VGA_R(VGA_R),
  .VGA_G(VGA_G),
  .VGA_B(VGA_B),
  .VGA_HS(VGA_HS),
  .VGA_VS(VGA_VS),
  .VGA_DE(VGA_DE),
  .VGA_PCLK(VGA_PCLK)
);

always #5 pixel_clk = ~pixel_clk;

task check_bundle;
  input [7:0] expected_r;
  input [7:0] expected_g;
  input [7:0] expected_b;
  input expected_hs;
  input expected_vs;
  input expected_de;
  begin
    @(posedge pixel_clk);
    #1;
    if ({VGA_R, VGA_G, VGA_B, VGA_HS, VGA_VS, VGA_DE} !==
        {expected_r, expected_g, expected_b, expected_hs, expected_vs, expected_de}) begin
      errors = errors + 1;
      $display("OUTPUT BUNDLE MISMATCH got=%02x/%02x/%02x %b%b%b expected=%02x/%02x/%02x %b%b%b",
               VGA_R, VGA_G, VGA_B, VGA_HS, VGA_VS, VGA_DE,
               expected_r, expected_g, expected_b, expected_hs, expected_vs,
               expected_de);
    end
    if (VGA_PCLK !== 1'b1) begin
      errors = errors + 1;
      $display("FORWARDED PCLK was not high after pixel-clock rising edge");
    end
    @(negedge pixel_clk);
    #1;
    if (VGA_PCLK !== 1'b0) begin
      errors = errors + 1;
      $display("FORWARDED PCLK was not low after pixel-clock falling edge");
    end
    if ({VGA_R, VGA_G, VGA_B, VGA_HS, VGA_VS, VGA_DE} !==
        {expected_r, expected_g, expected_b, expected_hs, expected_vs, expected_de}) begin
      errors = errors + 1;
      $display("OUTPUT BUNDLE CHANGED ON SiI9022 FALLING SAMPLE EDGE");
    end
  end
endtask

initial begin
  @(negedge pixel_clk);
  rgb_r_in = 8'h12;
  rgb_g_in = 8'h34;
  rgb_b_in = 8'h56;
  hsync_in = 1'b1;
  vsync_in = 1'b0;
  de_in = 1'b1;
  check_bundle(8'h12, 8'h34, 8'h56, 1'b1, 1'b0, 1'b1);

  rgb_r_in = 8'hab;
  rgb_g_in = 8'hcd;
  rgb_b_in = 8'hef;
  hsync_in = 1'b0;
  vsync_in = 1'b1;
  de_in = 1'b0;
  check_bundle(8'hab, 8'hcd, 8'hef, 1'b0, 1'b1, 1'b0);

  if (errors == 0)
    $display("RESULT OUTPUT_STAGE PASS");
  else
    $display("RESULT OUTPUT_STAGE FAIL errors=%0d", errors);
  $finish;
end
endmodule
