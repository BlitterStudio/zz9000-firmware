// RasterIX
// https://github.com/ToNi3141/RasterIX
// Copyright (c) 2026 ToNi3141

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

// Texture reader context which holds the four texel lanes of the current fragment.
// Counterpart is the TextureReaderController which manages the lanes.
// Every command updates all lanes at once: a lane keeps its texel, takes the
// texel of another lane or takes one of the two texels which arrive from the
// cache with the command (see TextureReaderController for the lane select).
// The lanes are also the output registers. They only change while no sample
// is waiting for the consumer.
// Pipelined: yes
// Depth: 1
module TextureReaderContext #(
    parameter TEXEL_WIDTH = 16
)
(
    input  wire                             aclk,
    input  wire                             resetn,

    // Interface to the TextureReaderController
    input  wire [11 : 0]                    s_trc_lane_sel,
    input  wire [2 * TEXEL_WIDTH - 1 : 0]   s_trc_texel,    // { slot B, slot A }
    input  wire                             s_trc_sample,   // 1 = emit the lanes
    input  wire                             s_trc_valid,
    output reg                              s_trc_ready,
    
    // Texture Read
    output reg  [TEXEL_WIDTH - 1 : 0]       m_tr_texel_00,
    output reg  [TEXEL_WIDTH - 1 : 0]       m_tr_texel_01,
    output reg  [TEXEL_WIDTH - 1 : 0]       m_tr_texel_10,
    output reg  [TEXEL_WIDTH - 1 : 0]       m_tr_texel_11,
    output reg                              m_tr_valid,
    input  wire                             m_tr_ready
);
    reg                             rSkidValid;
    reg  [11 : 0]                   rSkidLaneSel;
    reg  [2 * TEXEL_WIDTH - 1 : 0]  rSkidTexel;
    reg                             rSkidSample;

    wire [11 : 0]                   wLaneSel = rSkidValid ? rSkidLaneSel : s_trc_lane_sel;
    wire [2 * TEXEL_WIDTH - 1 : 0]  wTexel   = rSkidValid ? rSkidTexel   : s_trc_texel;
    wire                            wSample  = rSkidValid ? rSkidSample  : s_trc_sample;
    wire                            wValid   = rSkidValid ? 1'b1         : s_trc_valid;

    function [TEXEL_WIDTH - 1 : 0] NextLane;
        input [ 2 : 0]                  select;
        input [TEXEL_WIDTH - 1 : 0]     lane00;
        input [TEXEL_WIDTH - 1 : 0]     lane01;
        input [TEXEL_WIDTH - 1 : 0]     lane10;
        input [TEXEL_WIDTH - 1 : 0]     lane11;
        input [2 * TEXEL_WIDTH - 1 : 0] loaded;
        begin
            case (select)
                3'b000:  NextLane = lane00;
                3'b001:  NextLane = lane01;
                3'b010:  NextLane = lane10;
                3'b011:  NextLane = lane11;
                3'b100:  NextLane = loaded[0 +: TEXEL_WIDTH];
                default: NextLane = loaded[TEXEL_WIDTH +: TEXEL_WIDTH];
            endcase
        end
    endfunction

    always @(posedge aclk)
    begin
        if (!resetn)
        begin
            m_tr_valid <= 1'b0;
            s_trc_ready <= 1'b1;
            rSkidValid <= 1'b0;
            /*
             * Nearest filtering requests only texel 00, but the registered
             * sample is four-wide.  Reset every cached lane so the unused
             * neighbours cannot inject an unknown into downstream texenv
             * pipeline state before a linear request has populated them.
             */
            m_tr_texel_00 <= { TEXEL_WIDTH { 1'b0 } };
            m_tr_texel_01 <= { TEXEL_WIDTH { 1'b0 } };
            m_tr_texel_10 <= { TEXEL_WIDTH { 1'b0 } };
            m_tr_texel_11 <= { TEXEL_WIDTH { 1'b0 } };
        end
        else
        begin
            if (!m_tr_valid || (m_tr_valid && m_tr_ready))
            begin
                if (wValid)
                begin
                    m_tr_texel_00 <= NextLane(wLaneSel[0 +: 3], m_tr_texel_00, m_tr_texel_01, m_tr_texel_10, m_tr_texel_11, wTexel);
                    m_tr_texel_01 <= NextLane(wLaneSel[3 +: 3], m_tr_texel_00, m_tr_texel_01, m_tr_texel_10, m_tr_texel_11, wTexel);
                    m_tr_texel_10 <= NextLane(wLaneSel[6 +: 3], m_tr_texel_00, m_tr_texel_01, m_tr_texel_10, m_tr_texel_11, wTexel);
                    m_tr_texel_11 <= NextLane(wLaneSel[9 +: 3], m_tr_texel_00, m_tr_texel_01, m_tr_texel_10, m_tr_texel_11, wTexel);
                    m_tr_valid <= wSample;

                    if (rSkidValid)
                    begin
                        rSkidValid <= 1'b0;
                        s_trc_ready <= 1'b1;
                    end
                end
                else
                begin
                    m_tr_valid <= 1'b0;
                end
            end
            else
            begin
                if (!rSkidValid)
                begin
                    rSkidLaneSel  <= s_trc_lane_sel;
                    rSkidTexel    <= s_trc_texel;
                    rSkidSample   <= s_trc_sample;
                    rSkidValid    <= s_trc_valid;
                    s_trc_ready   <= !s_trc_valid;
                end
            end
        end
    end
endmodule 
