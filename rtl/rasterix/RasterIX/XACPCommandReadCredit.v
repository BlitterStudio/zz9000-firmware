// SPDX-License-Identifier: GPL-3.0-or-later
// XACP-GPU G4C command-read AXI credit buffer (independently authored 2026).
// Inspired by the read-credit architecture used by Ruben Aparicio's BigMig
// (bm_axi_rd_credit.v); no BigMig source code is copied.
//
// AXI4 read channels only. Insert after command commonAxiAdapter (64-bit master),
// before S0 of RasterIX's mainXBar. All non-command channels are unchanged.
// Capacity is reserved on accepted AR for every expected R beat (ARLEN+1).
// The downstream R channel is always ready while out of reset. With a compliant
// AXI responder, reservations ensure the FIFO cannot overflow even if the
// command consumer remains blocked indefinitely.
module XACPCommandReadCredit #(
    parameter integer ID_WIDTH = 5,
    parameter integer ADDR_WIDTH = 32,
    parameter integer DATA_WIDTH = 64,
    parameter integer DEPTH_LOG2 = 6
)(
    input wire clk,
    input wire resetn,
    input wire [ID_WIDTH-1:0] s_arid,
    input wire [ADDR_WIDTH-1:0] s_araddr,
    input wire [7:0] s_arlen,
    input wire [2:0] s_arsize,
    input wire [1:0] s_arburst,
    input wire s_arlock,
    input wire [3:0] s_arcache,
    input wire [2:0] s_arprot,
    input wire s_arvalid,
    output wire s_arready,
    output wire [ID_WIDTH-1:0] s_rid,
    output wire [DATA_WIDTH-1:0] s_rdata,
    output wire [1:0] s_rresp,
    output wire s_rlast,
    output wire s_rvalid,
    input wire s_rready,
    output wire [ID_WIDTH-1:0] m_arid,
    output wire [ADDR_WIDTH-1:0] m_araddr,
    output wire [7:0] m_arlen,
    output wire [2:0] m_arsize,
    output wire [1:0] m_arburst,
    output wire m_arlock,
    output wire [3:0] m_arcache,
    output wire [2:0] m_arprot,
    output wire m_arvalid,
    input wire m_arready,
    input wire [ID_WIDTH-1:0] m_rid,
    input wire [DATA_WIDTH-1:0] m_rdata,
    input wire [1:0] m_rresp,
    input wire m_rlast,
    input wire m_rvalid,
    output wire m_rready
);
    localparam integer CAPACITY = (1 << DEPTH_LOG2);
    localparam integer ITEM_WIDTH = ID_WIDTH + DATA_WIDTH + 3;
    localparam integer COUNT_WIDTH = DEPTH_LOG2 + 1;
    wire [8:0] beats_needed = {1'b0, s_arlen} + 9'd1;
    reg [COUNT_WIDTH-1:0] reserved_count;
    reg [COUNT_WIDTH-1:0] rd_ptr;
    reg [COUNT_WIDTH-1:0] wr_ptr;
    wire [9:0] needed_total = {1'b0, beats_needed};
    wire [9:0] reserved_extended = {{(10-COUNT_WIDTH){1'b0}}, reserved_count};
    wire room = (reserved_extended + needed_total <= CAPACITY);
    wire ar_handshake = resetn && s_arvalid && s_arready;

    assign m_arid = s_arid;
    assign m_araddr = s_araddr;
    assign m_arlen = s_arlen;
    assign m_arsize = s_arsize;
    assign m_arburst = s_arburst;
    assign m_arlock = s_arlock;
    assign m_arcache = s_arcache;
    assign m_arprot = s_arprot;
    assign m_arvalid = resetn && s_arvalid && room;
    assign s_arready = resetn && m_arready && room;

    reg [ITEM_WIDTH-1:0] mem [0:CAPACITY-1];
    wire [ITEM_WIDTH-1:0] head = mem[rd_ptr[DEPTH_LOG2-1:0]];
    assign {s_rid, s_rdata, s_rresp, s_rlast} = head;
    assign s_rvalid = resetn && (wr_ptr != rd_ptr);
    assign m_rready = resetn;

    wire push = resetn && m_rvalid && m_rready;
    wire pop = s_rvalid && s_rready;
    wire [COUNT_WIDTH-1:0] add_beats = beats_needed[COUNT_WIDTH-1:0];
    always @(posedge clk) begin
        if (!resetn) begin
            rd_ptr <= {COUNT_WIDTH{1'b0}};
            wr_ptr <= {COUNT_WIDTH{1'b0}};
            reserved_count <= {COUNT_WIDTH{1'b0}};
        end else begin
            if (push) begin
                mem[wr_ptr[DEPTH_LOG2-1:0]] <= {m_rid, m_rdata, m_rresp, m_rlast};
                wr_ptr <= wr_ptr + 1'b1;
            end
            if (pop)
                rd_ptr <= rd_ptr + 1'b1;
            reserved_count <= reserved_count
                              + (ar_handshake ? add_beats : {COUNT_WIDTH{1'b0}})
                              - (pop ? {{(COUNT_WIDTH-1){1'b0}}, 1'b1} : {COUNT_WIDTH{1'b0}});
        end
    end

    initial begin
        if (DEPTH_LOG2 < 3 || DEPTH_LOG2 > 8)
            $error("XACPCommandReadCredit: DEPTH_LOG2 out of supported range 3..8");
        if (DATA_WIDTH != 64)
            $error("XACPCommandReadCredit: G4C expects 64-bit post-adapter AXI");
    end
endmodule
