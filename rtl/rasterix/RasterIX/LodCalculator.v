// RasterIX
// https://github.com/ToNi3141/RasterIX
// Copyright (c) 2023 ToNi3141

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

// Calculates the lod level of a texel. It expects on additional texel
// which is incremented by x and y in screenspace
//
//  -------------------------
// | this pixel |            | Pixel used for the rendeing
//  -------------------------
// |            | this pixel | Pixel only used for mipmap calculation
//  -------------------------
//
// Pipelined: yes
// Depth: 2 cycles
module LodCalculator
#(
    parameter USER_WIDTH = 1
)
(
    input  wire                         aclk,
    input  wire                         resetn,

    input  wire                         confEnable,

    output wire                         s_ready,
    input  wire                         s_valid,
    input  wire [USER_WIDTH - 1 : 0]    s_user,
    input  wire [ 3 : 0]                s_textureSizeWidth, 
    input  wire [ 3 : 0]                s_textureSizeHeight,
    input  wire [31 : 0]                s_texelS, // S16.15
    input  wire [31 : 0]                s_texelT, // S16.15
    input  wire [31 : 0]                s_texelSxy, // S16.15
    input  wire [31 : 0]                s_texelTxy, // S16.15

    input  wire                         m_ready,
    output reg                          m_valid,
    output reg  [USER_WIDTH - 1 : 0]    m_user,
    output reg  [ 3 : 0]                m_lod
);
    wire ce;
    assign ce = m_ready;
    assign s_ready = m_ready;

    wire [31 : 0] diffS = s_texelS - s_texelSxy;
    wire [31 : 0] diffT = s_texelT - s_texelTxy;
    wire [14 : 0] diffUnsignedS = diffS[31] ? ~diffS[0 +: 15] + 1 : diffS[0 +: 15];
    wire [14 : 0] diffUnsignedT = diffT[31] ? ~diffT[0 +: 15] + 1 : diffT[0 +: 15];
    // ZZ9000: the texel step is kept to half a texel, and the level is
    // log2 of it rounded at 1.5 * 2^k, near OpenGL's sqrt(2) * 2^k.  Upstream
    // took whole texels and picked level 1 for any step of one texel, so a
    // texture drawn 1:1 (2D text, HUD) was sampled from its half-size level.
    // Two register stages (step, then level) keep the wider compare off the
    // 100 MHz critical path; every output travels with its user data.
    wire [ 8 : 0] diffU = diffUnsignedS[6 +: 9] >> (8 - s_textureSizeWidth);
    wire [ 8 : 0] diffV = diffUnsignedT[6 +: 9] >> (8 - s_textureSizeHeight);

    reg [ 8 : 0]                step0_diffU;
    reg [ 8 : 0]                step0_diffV;
    reg                         step0_enable;
    reg                         step0_valid;
    reg [USER_WIDTH - 1 : 0]    step0_user;

    always @(posedge aclk)
    if (ce) begin
        step0_diffU <= diffU;
        step0_diffV <= diffV;
        step0_enable <= confEnable;
        step0_valid <= s_valid;
        step0_user <= s_user;
    end

    wire [ 8 : 0] diffMax = (step0_diffU > step0_diffV) ? step0_diffU : step0_diffV;
    always @(posedge aclk)
    if (ce) begin : Mux
        reg [ 3 : 0] lodReg;
        casez (diffMax) // half texels
            9'b00000000?: lodReg = 0;
            9'b000000010: lodReg = 0;
            9'b000000011: lodReg = 1;
            9'b00000010?: lodReg = 1;
            9'b00000011?: lodReg = 2;
            9'b0000010??: lodReg = 2;
            9'b0000011??: lodReg = 3;
            9'b000010???: lodReg = 3;
            9'b000011???: lodReg = 4;
            9'b00010????: lodReg = 4;
            9'b00011????: lodReg = 5;
            9'b0010?????: lodReg = 5;
            9'b0011?????: lodReg = 6;
            9'b010??????: lodReg = 6;
            9'b011??????: lodReg = 7;
            9'b10???????: lodReg = 7;
            9'b11???????: lodReg = 8;
        endcase

        if (step0_enable)
        begin
            m_lod <= lodReg;
        end
        else
        begin
            m_lod <= 0;
        end
        m_valid <= step0_valid;
        m_user <= step0_user;
    end
endmodule