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

// Direct-mapped texture cache controller which manages the cache.
// It checks for cache hits and misses and loads new data into the 
// context or overwrites data from the context (TextureCacheDirectMappedContext).
//
// A request has two slots (A and B), each of them reads one texel or is
// unused. When all used slots hit, the controller issues one read of both
// slots. Otherwise it serves the request slot by slot, in order: it loads the
// cache line of a missing slot, reads a slot which hits on its own (without
// `emit`), and issues the last read with `emit`. This keeps two slots, which
// map to the same cache line index, from evicting each other.
// Pipelined: yes
// Depth: 1
module TextureCacheDirectMappedController #(
    parameter CACHE_SIZE = 1024,
    parameter CACHE_LINE_SIZE = 32,
    localparam CACHE_LINES = CACHE_SIZE / CACHE_LINE_SIZE,

    parameter DATA_WIDTH = 16,
    parameter ID_WIDTH = 4,
    parameter ADDR_WIDTH = 18,

    localparam TAG_WIDTH = ADDR_WIDTH - $clog2(CACHE_LINE_SIZE) - $clog2(CACHE_LINES),
    localparam TAG_ENTRY_WIDTH = TAG_WIDTH + 1,
    localparam INDEX_WIDTH = $clog2(CACHE_LINES),
    localparam ARSIZE = $clog2(DATA_WIDTH / 8)
)
(
    input  wire                             aclk,
    input  wire                             resetn,

    input  wire                             invalidate,
    input  wire                             enable,

    // Input interface
    input  wire [ 1 : 0]                    s_tc_slot_valid, // Slots which read a texel { B, A }
    input  wire [2 * ADDR_WIDTH - 1 : 0]    s_tc_addr,       // Byte address of slot { B, A }
    input  wire                             s_tc_valid,
    output reg                              s_tc_ready,

    output reg                              m_tc_valid,
    input  wire                             m_tc_ready,
    output reg                              m_tc_cmd,
    output reg  [ 1 : 0]                    m_tc_read,       // Slots which are read { B, A }
    output reg                              m_tc_emit,       // Last operation of the request
    output reg  [2 * ADDR_WIDTH - 1 : 0]    m_tc_addr,       // Byte address of slot { B, A }, a load uses slot A

    // Output interface
    output wire [ID_WIDTH - 1 : 0]          m_axi_arid,
    output reg  [ADDR_WIDTH - 1 : 0]        m_axi_araddr,
    output wire [ 7 : 0]                    m_axi_arlen,
    output wire [ 2 : 0]                    m_axi_arsize,
    output wire [ 1 : 0]                    m_axi_arburst,
    output wire                             m_axi_arlock,
    output wire [ 3 : 0]                    m_axi_arcache,
    output wire [ 2 : 0]                    m_axi_arprot,
    output reg                              m_axi_arvalid,
    input  wire                             m_axi_arready
);
    localparam READ_CACHE_ENTRY = 1'b0;
    localparam LOAD_CACHE_LINE = 1'b1;

    function [TAG_WIDTH - 1 : 0] GetTagFromAddress;
        input [ADDR_WIDTH - 1 : 0] addr;
        begin
            GetTagFromAddress = addr[$clog2(CACHE_LINE_SIZE) + $clog2(CACHE_LINES) +: TAG_WIDTH];
        end
    endfunction

    function [INDEX_WIDTH - 1 : 0] GetIndexFromAddress;
        input [ADDR_WIDTH - 1 : 0] addr;
        begin
            GetIndexFromAddress = addr[$clog2(CACHE_LINE_SIZE) +: $clog2(CACHE_LINES)];
        end
    endfunction

    function [ADDR_WIDTH - 1 : 0] GetBaseAddress;
        input [ADDR_WIDTH - 1 : 0] addr;
        begin
            GetBaseAddress = addr & { {(ADDR_WIDTH - $clog2(CACHE_LINE_SIZE)) { 1'b1 }},
                                      {$clog2(CACHE_LINE_SIZE) { 1'b0 }} };
        end
    endfunction

    function [0 : 0] TagMatch;
        input [TAG_ENTRY_WIDTH - 1 : 0] tagEntry;
        input [ADDR_WIDTH - 1 : 0] addr;
        begin
            TagMatch = !enable || ((tagEntry[0 +: TAG_WIDTH] == GetTagFromAddress(addr)) && tagEntry[TAG_ENTRY_WIDTH - 1]);
        end
    endfunction

    assign m_axi_arid = 0;
    assign m_axi_arlen = (CACHE_LINE_SIZE / (DATA_WIDTH / 8)) - 1;
    assign m_axi_arsize = ARSIZE[0 +: 3];
    assign m_axi_arburst = 2'b01; // INCR burst type
    assign m_axi_arlock = 0;
    assign m_axi_arcache = 0;
    assign m_axi_arprot = 0;

    // The tag memory exists twice, one copy for each slot. Both are written alike.
    reg [TAG_ENTRY_WIDTH - 1 : 0]   rTagEntriesA [0 : CACHE_LINES - 1];
    reg [TAG_ENTRY_WIDTH - 1 : 0]   rTagEntriesB [0 : CACHE_LINES - 1];

    reg                             rInvalidate;
    reg [INDEX_WIDTH - 1 : 0]       rI;

    reg                             rSkidValid;
    reg  [ 1 : 0]                   rSkidSlots;
    reg  [2 * ADDR_WIDTH - 1 : 0]   rSkidAddr;
    reg                             rLoadLinePending;

    wire [ 1 : 0]                   wSlots = (rSkidValid) ? rSkidSlots : s_tc_slot_valid;
    wire [2 * ADDR_WIDTH - 1 : 0]   wAddr = (rSkidValid) ? rSkidAddr : s_tc_addr;
    wire [ADDR_WIDTH - 1 : 0]       wAddrA = wAddr[0 +: ADDR_WIDTH];
    wire [ADDR_WIDTH - 1 : 0]       wAddrB = wAddr[ADDR_WIDTH +: ADDR_WIDTH];

    wire                            wHitA = !wSlots[0] || TagMatch(rTagEntriesA[GetIndexFromAddress(wAddrA)], wAddrA);
    wire                            wHitB = !wSlots[1] || TagMatch(rTagEntriesB[GetIndexFromAddress(wAddrB)], wAddrB);
    // Write port of the tag memory. The invalidation sweep writes the entry
    // rI directly. A miss registers its update (rTagUpdate...) and the entry
    // is written in the following cycle, which is before the request is
    // looked up again: the load command of the miss stays in the output
    // register for at least one cycle, and the request is only looked up
    // after that command has been taken.
    reg                             rTagUpdate;
    reg [INDEX_WIDTH - 1 : 0]       rTagUpdateIndex;
    reg [TAG_ENTRY_WIDTH - 1 : 0]   rTagUpdateEntry;
    always @(posedge aclk)
    begin
        if (resetn && (invalidate || rInvalidate))
        begin
            rTagEntriesA[rI] <= { TAG_ENTRY_WIDTH { 1'b0 } };
            rTagEntriesB[rI] <= { TAG_ENTRY_WIDTH { 1'b0 } };
        end
        else if (rTagUpdate)
        begin
            rTagEntriesA[rTagUpdateIndex] <= rTagUpdateEntry;
            rTagEntriesB[rTagUpdateIndex] <= rTagUpdateEntry;
        end
    end

    always @(posedge aclk)
    begin
        if (!resetn) 
        begin
            rInvalidate <= 1'b1;
            rI <= { INDEX_WIDTH { 1'b0 } };
            rSkidValid <= 1'b0;
            rLoadLinePending <= 1'b0;
            s_tc_ready <= 1'b0;
            m_tc_valid <= 1'b0;
            m_axi_arvalid <= 1'b0;
            rTagUpdate <= 1'b0;
        end 
        else 
        begin
            rTagUpdate <= 1'b0;
            if (invalidate || rInvalidate)
            begin
                rInvalidate <= 1'b1;
                if (rI != { INDEX_WIDTH { 1'b1 } })
                begin
                    rI <= rI + 1'b1;
                    // The tag memory is cleared by the write port below
                end
                else
                begin
                    // The tag memory is cleared by the write port below
                    rInvalidate <= 1'b0;
                    rI <= { INDEX_WIDTH { 1'b0 } };
                    s_tc_ready <= !rSkidValid;
                end
                
                if (s_tc_valid && s_tc_ready)
                begin
                    rSkidValid <= 1'b1;
                    rSkidSlots <= s_tc_slot_valid;
                    rSkidAddr <= s_tc_addr;
                    s_tc_ready <= 1'b0;
                end
                if (m_tc_valid && m_tc_ready)
                begin
                    m_tc_valid <= 1'b0;
                end
            end
            else if (rLoadLinePending && m_tc_valid && m_tc_ready)
            begin
                rLoadLinePending <= 1'b0;
                m_tc_valid <= 1'b0;
            end
            else if ((s_tc_valid || rSkidValid) && (!m_tc_valid || m_tc_ready) && !m_axi_arvalid)
            begin            
                m_tc_valid <= 1'b1;
                m_tc_addr <= wAddr;
                m_tc_cmd <= READ_CACHE_ENTRY;
                m_tc_read <= wSlots;
                m_tc_emit <= 1'b1;
                rSkidValid <= 1'b0;

                s_tc_ready <= 1'b1;

                if (!wHitA)
                begin
                    // Load the line of slot A. The request is served again afterwards.
                    m_tc_cmd <= LOAD_CACHE_LINE;
                    
                    rSkidValid <= 1'b1;
                    rSkidSlots <= wSlots;
                    rSkidAddr <= wAddr;
                    rLoadLinePending <= 1'b1;
                    s_tc_ready <= 1'b0;
                    
                    m_axi_arvalid <= 1'b1;
                    m_axi_araddr <= GetBaseAddress(wAddrA);
                    
                    rTagUpdate <= 1'b1;
                    rTagUpdateIndex <= GetIndexFromAddress(wAddrA);
                    rTagUpdateEntry <= { 1'b1, GetTagFromAddress(wAddrA) };
                end
                else if (!wHitB)
                begin
                    if (wSlots[0])
                    begin
                        // Slot A hits. Read it before the line of slot B is loaded, because
                        // this load might replace the line of slot A.
                        m_tc_read <= 2'b01;
                        m_tc_emit <= 1'b0;

                        rSkidValid <= 1'b1;
                        rSkidSlots <= { wSlots[1], 1'b0 };
                        rSkidAddr <= wAddr;
                        s_tc_ready <= 1'b0;
                    end
                    else
                    begin
                        // Load the line of slot B. The request is served again afterwards.
                        m_tc_cmd <= LOAD_CACHE_LINE;
                        m_tc_addr <= { wAddrB, wAddrB };

                        rSkidValid <= 1'b1;
                        rSkidSlots <= wSlots;
                        rSkidAddr <= wAddr;
                        rLoadLinePending <= 1'b1;
                        s_tc_ready <= 1'b0;

                        m_axi_arvalid <= 1'b1;
                        m_axi_araddr <= GetBaseAddress(wAddrB);

                        rTagUpdate <= 1'b1;
                        rTagUpdateIndex <= GetIndexFromAddress(wAddrB);
                        rTagUpdateEntry <= { 1'b1, GetTagFromAddress(wAddrB) };
                    end
                end
            end
            else if (m_tc_valid && !m_tc_ready && s_tc_valid && s_tc_ready)
            begin
                rSkidAddr <= wAddr;
                rSkidSlots <= wSlots;
                rSkidValid <= 1'b1;
                s_tc_ready <= 1'b0;
            end
            else if (!s_tc_valid && !rSkidValid && (!m_tc_valid || m_tc_ready))
            begin
                m_tc_valid <= 1'b0;
                s_tc_ready <= 1'b1;
                rSkidValid <= 1'b0;
            end
        end

        if (m_axi_arvalid && m_axi_arready)
        begin
            m_axi_arvalid <= 1'b0;
        end
    end

endmodule
