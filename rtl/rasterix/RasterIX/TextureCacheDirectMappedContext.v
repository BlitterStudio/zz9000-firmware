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

// Direct-mapped texture cache context which manages the cache and handles read requests.
// This is the counterpart to the TextureCacheDirectMappedController which manages the cache.
//
// A read serves up to two texels (slot A and slot B). The memory exists twice
// (both copies are written with the same data), so both texels are read in
// the same cycle. The memory is read with a registered output; the texel is
// selected from the word in the following stage. A request which the
// controller split into several reads (read of slot A without `emit`, then
// read of slot B with `emit`) is merged into one output transfer: the output
// register of a slot keeps its texel until the slot is read again.
// Pipelined: yes
// Depth: 2
module TextureCacheDirectMappedContext #(
    parameter TEXEL_WIDTH = 16,

    parameter CACHE_SIZE = 1024,
    parameter CACHE_LINE_SIZE = 32,
    localparam CACHE_LINES = CACHE_SIZE / CACHE_LINE_SIZE,
    // This option enables fetching of cache lines while they are loaded.
    // This requires dual port RAM. Is this option disabled, the cache waits
    // until the while cache line is loaded before serving new requests.
    // In this case, single port memory is sufficient.
    parameter ENABLE_EARLY_FETCH = 0,

    parameter DATA_WIDTH = 32,
    parameter ID_WIDTH = 4,
    parameter ADDR_WIDTH = 18,

    localparam LG_DATA_BYTES = $clog2(DATA_WIDTH / 8),
    localparam LG_CACHE_WORDS = $clog2(CACHE_SIZE / (DATA_WIDTH / 8)),
    localparam LINE_WORDS_LG = $clog2(CACHE_LINE_SIZE / (DATA_WIDTH / 8)),
    localparam TAG_WIDTH = ADDR_WIDTH - $clog2(CACHE_LINE_SIZE) - $clog2(CACHE_LINES),
    localparam TEXEL_OFFSET_WIDTH = (DATA_WIDTH / TEXEL_WIDTH) > 1 ? $clog2(DATA_WIDTH / TEXEL_WIDTH) : 1
)
(
    input  wire                             aclk,
    input  wire                             resetn,

    // Input interface
    input  wire                             s_tc_valid,
    output reg                              s_tc_ready,
    input  wire                             s_tc_cmd,
    input  wire [ 1 : 0]                    s_tc_read,      // Slots which are read { B, A }
    input  wire                             s_tc_emit,      // The output transfer is complete after this read
    input  wire [2 * ADDR_WIDTH - 1 : 0]    s_tc_addr,      // Byte address of slot { B, A }

    output reg  [2 * TEXEL_WIDTH - 1 : 0]   m_tc_texel,     // { slot B, slot A }
    output reg                              m_tc_valid,
    input  wire                             m_tc_ready,

    // AXI input interface
    input  wire [ID_WIDTH - 1 : 0]          m_axi_rid,
    input  wire [DATA_WIDTH - 1 : 0]        m_axi_rdata,
    input  wire [ 1 : 0]                    m_axi_rresp,
    input  wire                             m_axi_rlast,
    input  wire                             m_axi_rvalid,
    output reg                              m_axi_rready
);
    localparam READ_CACHE_ENTRY = 1'b0;
    localparam LOAD_CACHE_LINE = 1'b1;

    // The memory exists twice, one copy for each read slot.
    (* ram_style = "block" *) reg [DATA_WIDTH - 1 : 0] rCacheMemoryA [0 : (CACHE_SIZE / (DATA_WIDTH / 8)) - 1];
    (* ram_style = "block" *) reg [DATA_WIDTH - 1 : 0] rCacheMemoryB [0 : (CACHE_SIZE / (DATA_WIDTH / 8)) - 1];

    function [ADDR_WIDTH - 1 : 0] GetCacheGroupAddress;
        input [ADDR_WIDTH - 1 : 0] addr;
        begin
            GetCacheGroupAddress = { 
                { (ADDR_WIDTH - $clog2(CACHE_LINE_SIZE) - $clog2(CACHE_LINES)) { 1'b0 } }, 
                addr[$clog2(CACHE_LINE_SIZE) +: $clog2(CACHE_LINES)], 
                { ( $clog2(CACHE_LINE_SIZE)) { 1'b0 } } 
            };
        end
    endfunction

    function [LG_CACHE_WORDS - 1 : 0] GetWordAddress;
        input [ADDR_WIDTH - 1 : 0] addr;
        begin
            GetWordAddress = addr[LG_DATA_BYTES +: LG_CACHE_WORDS];
        end
    endfunction

    function [TEXEL_OFFSET_WIDTH - 1 : 0] GetTexelOffset;
        input [ADDR_WIDTH - 1 : 0] addr;
        begin
            if (DATA_WIDTH == TEXEL_WIDTH)
            begin
                GetTexelOffset = 0;
            end
            else
            begin
                GetTexelOffset = addr[$clog2(TEXEL_WIDTH / 8) +: TEXEL_OFFSET_WIDTH];
            end
        end
    endfunction

    function [TEXEL_WIDTH - 1 : 0] GetTexel;
        input [DATA_WIDTH - 1 : 0]          word;
        input [TEXEL_OFFSET_WIDTH - 1 : 0]  offset;
        reg   [DATA_WIDTH - 1 : 0]          shiftedWord;
        integer texelShift;
        begin
            if (DATA_WIDTH == TEXEL_WIDTH)
            begin
                texelShift = 0;
            end
            else
            begin
                texelShift = TEXEL_WIDTH * offset;
            end
            shiftedWord = word >> texelShift;
            GetTexel = shiftedWord[0 +: TEXEL_WIDTH];
        end
    endfunction

    reg                             rSkidValid;
    reg                             rSkidCmd;
    reg  [ 1 : 0]                   rSkidRead;
    reg                             rSkidEmit;
    reg  [2 * ADDR_WIDTH - 1 : 0]   rSkidAddr;

    reg  [ADDR_WIDTH - 1 : 0]       rI;
    reg  [ADDR_WIDTH - 1 : 0]       rAxiAddr;

    // Stage 1: the memory output registers and the description of the read
    reg                             rReadValid;
    reg  [ 1 : 0]                   rReadSlots;
    reg                             rReadEmit;
    reg  [TEXEL_OFFSET_WIDTH - 1 : 0] rReadOffsetA;
    reg  [TEXEL_OFFSET_WIDTH - 1 : 0] rReadOffsetB;
    reg  [DATA_WIDTH - 1 : 0]       rReadWordA;
    reg  [DATA_WIDTH - 1 : 0]       rReadWordB;

    wire                            wCmd  = rSkidValid ? rSkidCmd  : s_tc_cmd;
    wire [ 1 : 0]                   wRead = rSkidValid ? rSkidRead : s_tc_read;
    wire                            wEmit = rSkidValid ? rSkidEmit : s_tc_emit;
    wire [2 * ADDR_WIDTH - 1 : 0]   wAddr = rSkidValid ? rSkidAddr : s_tc_addr;
    wire [ADDR_WIDTH - 1 : 0]       wAddrA = wAddr[0 +: ADDR_WIDTH];
    wire [ADDR_WIDTH - 1 : 0]       wAddrB = wAddr[ADDR_WIDTH +: ADDR_WIDTH];

    // The output stage takes the read of the memory stage when it is free
    wire                            wOutputFree = !m_tc_valid || m_tc_ready;
    // The memory stage takes a new read when its read moves on
    wire                            wReadAdvance = !rReadValid || wOutputFree;

    // The word of a read is not loaded yet while the cache line is filled. rI is below the
    // line size while a fill runs, therefore only the word index within the line is compared.
    wire [LINE_WORDS_LG - 1 : 0]    wFillWord = rI[LG_DATA_BYTES +: LINE_WORDS_LG];
    wire                            wWaitFillA = m_axi_rready && (wFillWord <= wAddrA[LG_DATA_BYTES +: LINE_WORDS_LG]);
    wire                            wWaitFillB = m_axi_rready && (wFillWord <= wAddrB[LG_DATA_BYTES +: LINE_WORDS_LG]);
    wire                            wWaitFill = (wRead[0] && wWaitFillA) || (wRead[1] && wWaitFillB);

    wire                            wWrite = m_axi_rready && m_axi_rvalid;
    wire [LG_CACHE_WORDS - 1 : 0]   wWriteWord = GetWordAddress(GetCacheGroupAddress(rAxiAddr) + rI);

    // Both memories are read at the same time. The output registers hold their
    // value while the memory stage cannot advance. A word which is written
    // while it is read cannot be requested, because a read only waits for
    // words which are below the write pointer (see wWaitFill).
    always @(posedge aclk)
    begin
        if (wWrite)
        begin
            rCacheMemoryA[wWriteWord] <= m_axi_rdata;
            rCacheMemoryB[wWriteWord] <= m_axi_rdata;
        end
        if (wReadAdvance)
        begin
            rReadWordA <= rCacheMemoryA[GetWordAddress(wAddrA)];
            rReadWordB <= rCacheMemoryB[GetWordAddress(wAddrB)];
        end
    end

    always @(posedge aclk) 
    begin
        if (!resetn) 
        begin
            rSkidValid   <= 1'b0;
            s_tc_ready   <= 1'b1;
            m_tc_valid   <= 1'b0;
            m_axi_rready <= 1'b0;
            rReadValid   <= 1'b0;
        end 
        else 
        begin
            // Output stage: select the texels of the read
            if (wOutputFree)
            begin
                m_tc_valid <= rReadValid && rReadEmit;
                if (rReadValid && rReadSlots[0])
                begin
                    m_tc_texel[0 +: TEXEL_WIDTH] <= GetTexel(rReadWordA, rReadOffsetA);
                end
                if (rReadValid && rReadSlots[1])
                begin
                    m_tc_texel[TEXEL_WIDTH +: TEXEL_WIDTH] <= GetTexel(rReadWordB, rReadOffsetB);
                end
            end

            // Memory stage: the read of this stage moves to the output stage
            if (wReadAdvance)
            begin
                rReadValid <= 1'b0;
            end

            // Memory stage: take the next request
            if ((ENABLE_EARLY_FETCH || !m_axi_rready) && wReadAdvance)
            begin

                if (s_tc_valid || rSkidValid)
                begin
                    if (rSkidValid)
                    begin
                        rSkidValid <= 1'b0;
                        s_tc_ready   <= 1'b1;
                    end
                    
                    if (wCmd == LOAD_CACHE_LINE)
                    begin
                        if (m_axi_rready)
                        begin
                            rSkidAddr  <= wAddr;
                            rSkidCmd   <= wCmd;
                            rSkidRead  <= wRead;
                            rSkidEmit  <= wEmit;
                            rSkidValid <= 1'b1;
                            s_tc_ready   <= 1'b0;
                        end
                        else
                        begin
                            m_axi_rready <= 1'b1;
                            rI           <= { ADDR_WIDTH { 1'b0 } };
                            rAxiAddr     <= wAddrA;
                        end
                    end
                    else if (wCmd == READ_CACHE_ENTRY)
                    begin
                        if (wWaitFill)
                        begin
                            rSkidAddr  <= wAddr;
                            rSkidCmd   <= wCmd;
                            rSkidRead  <= wRead;
                            rSkidEmit  <= wEmit;
                            rSkidValid <= 1'b1;
                            s_tc_ready   <= 1'b0;
                        end
                        else
                        begin
                            rReadValid   <= 1'b1;
                            rReadSlots   <= wRead;
                            rReadEmit    <= wEmit;
                            rReadOffsetA <= GetTexelOffset(wAddrA);
                            rReadOffsetB <= GetTexelOffset(wAddrB);
                        end
                    end
                end
            end
            else
            begin
                if (!rSkidValid)
                begin
                    rSkidAddr  <= s_tc_addr;
                    rSkidCmd   <= s_tc_cmd;
                    rSkidRead  <= s_tc_read;
                    rSkidEmit  <= s_tc_emit;
                    rSkidValid <= s_tc_valid;
                    s_tc_ready   <= !s_tc_valid;
                end
            end

            if (wWrite)
            begin
                rI <= rI + (DATA_WIDTH / 8);
                if (rI == (CACHE_LINE_SIZE - (DATA_WIDTH / 8)))
                begin
                    m_axi_rready <= 1'b0;
                end
            end
        end
    end

endmodule
