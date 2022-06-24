`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 08/13/2021 09:10:18 PM
// Design Name: 
// Module Name: audio_clock
// Project Name: 
// Target Devices: 
// Tool Versions: 
// Description: 
// 
// Dependencies: 
// 
// Revision:
// Revision 0.01 - File Created
// Additional Comments:
// 
//////////////////////////////////////////////////////////////////////////////////


module audio_clock(
    input bclk_in,
    input fclk_in,
    output bclk_out,
    output mclk_out
    );
    
    reg [2:0] clkgen;
    reg [2:0] clkbuf;
    
    assign mclk_out = clkbuf[2];
    assign bclk_out = clkgen[2];
    
    always @(posedge fclk_in) begin
      clkbuf <= {clkbuf[1:0], bclk_in};
      
      if (clkbuf[1] == 1'b0 && clkbuf[2] == 1'b1)
        clkgen <= clkgen + 1'b1;
    end
    
endmodule
