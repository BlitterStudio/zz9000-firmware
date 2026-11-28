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

// Texture reader controller which manages the texture reader context and handles read requests.
//
// The context keeps one texel per lane (00, 01, 10, 11) and this controller
// tracks the texel address held in each lane.  A requested texel address
// that is already held in any lane is not fetched again: the lane contents
// are moved between lanes.  The remaining texels are fetched through two
// load slots, so a fragment costs one cycle when at most two texels are
// missing and two cycles otherwise.  A unit step of the sample quad in s or
// t misses two texels.
//
// Every command carries, per lane, the lane select of the new lane contents:
//   {1'b0, lane[1:0]}  take the old contents of lane `lane` (the own lane
//                      keeps its contents)
//   {1'b1, 2'b00}      take the texel loaded by slot A
//   {1'b1, 2'b01}      take the texel loaded by slot B
// All lanes are updated at once from the old contents.  `sample` marks the
// last command of a fragment.
// Pipelined: yes
// Depth: 1
module TextureReaderController #(
    parameter TEX_ADDR_WIDTH = 17,
    parameter TEXEL_WIDTH = 16,
    parameter ID_WIDTH = 4,
    localparam BYTE_ADDR_WIDTH = TEX_ADDR_WIDTH + 1,
    localparam ARSIZE = $clog2(TEXEL_WIDTH / 8)
)
(
    input  wire                                 aclk,
    input  wire                                 resetn,

    // Control
    input  wire                                 invalidate,
    input  wire                                 nearest,

    // Texture Read
    input  wire [TEX_ADDR_WIDTH - 1 : 0]        s_tr_texel_00,
    input  wire [TEX_ADDR_WIDTH - 1 : 0]        s_tr_texel_01,
    input  wire [TEX_ADDR_WIDTH - 1 : 0]        s_tr_texel_10,
    input  wire [TEX_ADDR_WIDTH - 1 : 0]        s_tr_texel_11,
    input  wire                                 s_tr_valid,
    output reg                                  s_tr_ready,

    // Fetch command for the texture cache and the TextureReaderContext
    output reg                                  m_trc_valid,
    input  wire                                 m_trc_ready,
    output reg  [ 1 : 0]                        m_trc_load,     // Slot is used { B, A }
    output reg  [2 * BYTE_ADDR_WIDTH - 1 : 0]   m_trc_addr,     // Byte address of slot { B, A }
    output reg  [11 : 0]                        m_trc_lane_sel, // 3 bits per lane, lane 00 in bits 2:0
    output reg                                  m_trc_sample    // 1 = the lanes are a complete fragment
);
    localparam INVALID_TEXEL_ADDR = { TEX_ADDR_WIDTH { 1'b1 } };

    reg [TEX_ADDR_WIDTH - 1 : 0] rTexel00;
    reg [TEX_ADDR_WIDTH - 1 : 0] rTexel01;
    reg [TEX_ADDR_WIDTH - 1 : 0] rTexel10;
    reg [TEX_ADDR_WIDTH - 1 : 0] rTexel11;

    reg                          rSkidValid;
    reg [TEX_ADDR_WIDTH - 1 : 0] rTexel00Skid;
    reg [TEX_ADDR_WIDTH - 1 : 0] rTexel01Skid;
    reg [TEX_ADDR_WIDTH - 1 : 0] rTexel10Skid;
    reg [TEX_ADDR_WIDTH - 1 : 0] rTexel11Skid;

    wire [TEX_ADDR_WIDTH - 1 : 0] wTexel00Next = (rSkidValid) ? rTexel00Skid : s_tr_texel_00;
    wire [TEX_ADDR_WIDTH - 1 : 0] wTexel01Next = (rSkidValid) ? rTexel01Skid : s_tr_texel_01;
    wire [TEX_ADDR_WIDTH - 1 : 0] wTexel10Next = (rSkidValid) ? rTexel10Skid : s_tr_texel_10;
    wire [TEX_ADDR_WIDTH - 1 : 0] wTexel11Next = (rSkidValid) ? rTexel11Skid : s_tr_texel_11;

    // eq<requested lane>_<held lane>: the requested texel is held in that lane
    wire eq0_0 = (wTexel00Next == rTexel00);
    wire eq0_1 = (wTexel00Next == rTexel01);
    wire eq0_2 = (wTexel00Next == rTexel10);
    wire eq0_3 = (wTexel00Next == rTexel11);
    wire eq1_0 = (wTexel01Next == rTexel00);
    wire eq1_1 = (wTexel01Next == rTexel01);
    wire eq1_2 = (wTexel01Next == rTexel10);
    wire eq1_3 = (wTexel01Next == rTexel11);
    wire eq2_0 = (wTexel10Next == rTexel00);
    wire eq2_1 = (wTexel10Next == rTexel01);
    wire eq2_2 = (wTexel10Next == rTexel10);
    wire eq2_3 = (wTexel10Next == rTexel11);
    wire eq3_0 = (wTexel11Next == rTexel00);
    wire eq3_1 = (wTexel11Next == rTexel01);
    wire eq3_2 = (wTexel11Next == rTexel10);
    wire eq3_3 = (wTexel11Next == rTexel11);

    // A lane has to be updated when it does not hold its requested texel.
    // Nearest filtering requests only texel 00.
    wire [3 : 0] wChange = {
        !(eq3_3 || nearest),
        !(eq2_2 || nearest),
        !(eq1_1 || nearest),
        !eq0_0
    };

    // Another lane holds the requested texel. The lowest lane wins.
    // Nearest filtering never forwards.
    wire [3 : 0] wForward = {
        !nearest && (eq3_0 || eq3_1 || eq3_2),
        !nearest && (eq2_0 || eq2_1 || eq2_3),
        !nearest && (eq1_0 || eq1_2 || eq1_3),
        !nearest && (eq0_1 || eq0_2 || eq0_3)
    };
    wire [1 : 0] wForwardLane0 = eq0_1 ? 2'd1 : (eq0_2 ? 2'd2 : 2'd3);
    wire [1 : 0] wForwardLane1 = eq1_0 ? 2'd0 : (eq1_2 ? 2'd2 : 2'd3);
    wire [1 : 0] wForwardLane2 = eq2_0 ? 2'd0 : (eq2_1 ? 2'd1 : 2'd3);
    wire [1 : 0] wForwardLane3 = eq3_0 ? 2'd0 : (eq3_1 ? 2'd1 : 2'd2);

    // Lanes which have to be loaded from the cache. The first two get a load
    // slot, the others are loaded by the next command of the fragment.
    wire [3 : 0] wLoad = wChange & ~wForward;
    wire [3 : 0] wSlotA = {
        wLoad[3] & ~(|wLoad[2 : 0]),
        wLoad[2] & ~(|wLoad[1 : 0]),
        wLoad[1] & ~wLoad[0],
        wLoad[0]
    };
    wire [3 : 0] wLoadRest = wLoad & ~wSlotA;
    wire [3 : 0] wSlotB = {
        wLoadRest[3] & ~(|wLoadRest[2 : 0]),
        wLoadRest[2] & ~(|wLoadRest[1 : 0]),
        wLoadRest[1] & ~wLoadRest[0],
        wLoadRest[0]
    };
    wire [3 : 0] wDeferred = wLoadRest & ~wSlotB;
    wire         wFinal = (wDeferred == 4'b0000);

    // Lanes updated by this command
    wire [3 : 0] wUpdate = wChange & ~wDeferred;

    wire [TEX_ADDR_WIDTH - 1 : 0] wSlotAAddr = ({ TEX_ADDR_WIDTH { wSlotA[0] } } & wTexel00Next)
                                             | ({ TEX_ADDR_WIDTH { wSlotA[1] } } & wTexel01Next)
                                             | ({ TEX_ADDR_WIDTH { wSlotA[2] } } & wTexel10Next)
                                             | ({ TEX_ADDR_WIDTH { wSlotA[3] } } & wTexel11Next);
    wire [TEX_ADDR_WIDTH - 1 : 0] wSlotBAddr = ({ TEX_ADDR_WIDTH { wSlotB[0] } } & wTexel00Next)
                                             | ({ TEX_ADDR_WIDTH { wSlotB[1] } } & wTexel01Next)
                                             | ({ TEX_ADDR_WIDTH { wSlotB[2] } } & wTexel10Next)
                                             | ({ TEX_ADDR_WIDTH { wSlotB[3] } } & wTexel11Next);

    wire [2 : 0] wLaneSel0 = (wChange[0] && wForward[0]) ? { 1'b0, wForwardLane0 } :
                             wSlotA[0]                   ? 3'b100 :
                             wSlotB[0]                   ? 3'b101 : 3'b000;
    wire [2 : 0] wLaneSel1 = (wChange[1] && wForward[1]) ? { 1'b0, wForwardLane1 } :
                             wSlotA[1]                   ? 3'b100 :
                             wSlotB[1]                   ? 3'b101 : 3'b001;
    wire [2 : 0] wLaneSel2 = (wChange[2] && wForward[2]) ? { 1'b0, wForwardLane2 } :
                             wSlotA[2]                   ? 3'b100 :
                             wSlotB[2]                   ? 3'b101 : 3'b010;
    wire [2 : 0] wLaneSel3 = (wChange[3] && wForward[3]) ? { 1'b0, wForwardLane3 } :
                             wSlotA[3]                   ? 3'b100 :
                             wSlotB[3]                   ? 3'b101 : 3'b011;

    always @(posedge aclk)
    begin
        if (!resetn || invalidate) 
        begin
            rTexel00 <= INVALID_TEXEL_ADDR;
            rTexel01 <= INVALID_TEXEL_ADDR;
            rTexel10 <= INVALID_TEXEL_ADDR;
            rTexel11 <= INVALID_TEXEL_ADDR;

            if (!resetn)
            begin
                s_tr_ready <= 1;
                rSkidValid <= 0;
                m_trc_valid <= 0;
            end
        end 
        else 
        begin
            if ((s_tr_valid || rSkidValid) && (!m_trc_valid || m_trc_ready))
            begin
                m_trc_valid <= 1;
                m_trc_sample <= wFinal;
                m_trc_load <= { |wSlotB, |wSlotA };
                m_trc_addr <= { wSlotBAddr, 1'b0, wSlotAAddr, 1'b0 };
                m_trc_lane_sel <= { wLaneSel3, wLaneSel2, wLaneSel1, wLaneSel0 };

                // The request is copied into the skid registers whenever it comes from the
                // input. This keeps the long compare chain off their clock enables. The copy
                // is only used when the fragment needs another command.
                if (!rSkidValid)
                begin
                    rTexel00Skid <= s_tr_texel_00;
                    rTexel01Skid <= s_tr_texel_01;
                    rTexel10Skid <= s_tr_texel_10;
                    rTexel11Skid <= s_tr_texel_11;
                end

                if (wFinal)
                begin
                    s_tr_ready <= 1;
                    rSkidValid <= 0;
                end
                else
                begin
                    // More than two texels have to be loaded. The fragment
                    // needs another command.
                    s_tr_ready <= 0;
                    rSkidValid <= 1;
                end

                // The lanes which are resolved by this command hold the requested texels afterwards.
                if (wUpdate[0]) rTexel00 <= wTexel00Next;
                if (wUpdate[1]) rTexel01 <= wTexel01Next;
                if (wUpdate[2]) rTexel10 <= wTexel10Next;
                if (wUpdate[3]) rTexel11 <= wTexel11Next;
            end
            else if (m_trc_valid && !m_trc_ready && s_tr_valid && s_tr_ready)
            begin
                rTexel00Skid <= s_tr_texel_00;
                rTexel01Skid <= s_tr_texel_01;
                rTexel10Skid <= s_tr_texel_10;
                rTexel11Skid <= s_tr_texel_11;
                rSkidValid <= 1;
                s_tr_ready <= 0;
            end
            else if (!s_tr_valid && !rSkidValid && (!m_trc_valid || m_trc_ready))
            begin
                m_trc_valid <= 0;
                s_tr_ready <= 1;
                rSkidValid <= 0;
            end
        end
    end
endmodule 
