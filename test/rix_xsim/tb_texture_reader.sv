`timescale 1ns/1ps
// Stand-alone randomized test of TextureReader (cache, MMU, lane context).
//
// A scoreboard knows the texel at every virtual texel address (a hash of the
// physical position behind the page table) and checks that every fragment
// returns exactly the four requested texels (texel 00 only for nearest
// filtering), in order.  Fragments follow the traversal patterns of bilinear
// sampling (unit steps in s and t, diagonals, half steps, back steps, jumps,
// equal addresses), with random gaps on the request channel, random back
// pressure on the texel channel and a random-latency AXI read model.  Between
// sequences the reader is drained and its page table is rewritten with a new
// random page permutation, which invalidates the cache.
//
// Plusargs: SEED=n  SEQUENCES=n  FRAGMENTS=n  (per sequence)
//           PERF=1  prints cycles per fragment for ideal memory and no stalls
module tb_texture_reader;
    parameter integer CACHE_BYTES = 8192;

    localparam integer TEXEL_WIDTH = 16;
    localparam integer DATA_WIDTH = 64;
    localparam integer ID_WIDTH = 6;
    localparam integer TEX_ADDR_WIDTH = 17;
    localparam integer PAGE_BYTES = 2048;
    localparam integer PAGES = 128;

    reg clk = 0;
    reg resetn = 0;
    always #5 clk = ~clk;

    reg                         enable = 1;
    reg                         nearest = 0;
    reg                         s_valid = 0;
    wire                        s_ready;
    reg  [TEX_ADDR_WIDTH-1:0]   a00, a01, a10, a11;
    wire                        m_valid;
    reg                         m_ready = 0;
    wire [TEXEL_WIDTH-1:0]      t00, t01, t10, t11;
    reg                         pt_valid = 0;
    wire                        pt_ready;
    reg                         pt_last = 0;
    reg  [31:0]                 pt_data = 0;

    wire [ID_WIDTH-1:0]         arid;
    wire [31:0]                 araddr;
    wire [7:0]                  arlen;
    wire [2:0]                  arsize;
    wire [1:0]                  arburst;
    wire                        arlock;
    wire [3:0]                  arcache;
    wire [2:0]                  arprot;
    wire                        arvalid;
    reg                         arready = 0;
    reg  [ID_WIDTH-1:0]         rid = 0;
    reg  [DATA_WIDTH-1:0]       rdata = 0;
    reg                         rlast = 0;
    reg                         rvalid = 0;
    wire                        rready;

    TextureReader #(
        .DATA_WIDTH(DATA_WIDTH),
        .TEXEL_WIDTH(TEXEL_WIDTH),
        .ID_WIDTH(ID_WIDTH),
        .ADDR_WIDTH(32),
        .PAGE_SIZE(PAGE_BYTES),
        .CACHE_SIZE(CACHE_BYTES)
    ) dut (
        .aclk(clk), .resetn(resetn), .enable(enable), .nearest(nearest),
        .s_tr_valid(s_valid), .s_tr_ready(s_ready),
        .s_tr_addr_00(a00), .s_tr_addr_01(a01), .s_tr_addr_10(a10), .s_tr_addr_11(a11),
        .m_tr_valid(m_valid), .m_tr_ready(m_ready),
        .m_tr_texel_00(t00), .m_tr_texel_01(t01), .m_tr_texel_10(t10), .m_tr_texel_11(t11),
        .s_axis_tvalid(pt_valid), .s_axis_tready(pt_ready), .s_axis_tlast(pt_last), .s_axis_tdata(pt_data),
        .m_axi_arid(arid), .m_axi_araddr(araddr), .m_axi_arlen(arlen), .m_axi_arsize(arsize),
        .m_axi_arburst(arburst), .m_axi_arlock(arlock), .m_axi_arcache(arcache), .m_axi_arprot(arprot),
        .m_axi_arvalid(arvalid), .m_axi_arready(arready),
        .m_axi_rid(rid), .m_axi_rdata(rdata), .m_axi_rresp(2'b00), .m_axi_rlast(rlast),
        .m_axi_rvalid(rvalid), .m_axi_rready(rready)
    );

    // ---------------------------------------------------------------- memory
    integer seed;
    integer page_phys [0:PAGES-1];   // physical page number of every virtual page
    integer region;                  // changes with every page table

    function [15:0] texel_at_phys;   // physical halfword index
        input integer half;
        reg [31:0] v;
        begin
            v = half * 32'h9e3779b1;
            v = v ^ (v >> 15);
            v = v * 32'h2c1b3c6d;
            v = v ^ (v >> 12);
            texel_at_phys = v[15:0] ^ v[31:16];
        end
    endfunction

    function [15:0] texel_at;        // virtual texel address
        input [TEX_ADDR_WIDTH-1:0] a;
        begin
            texel_at = texel_at_phys(page_phys[a / (PAGE_BYTES / 2)] * (PAGE_BYTES / 2) + (a % (PAGE_BYTES / 2)));
        end
    endfunction

    // ------------------------------------------------------------- AXI model
    integer ar_gap_pct;
    integer r_gap_pct;
    integer latency_min, latency_max;
    reg [31:0] rd_addr_q [0:63];
    integer    rd_ready_cycle_q [0:63];
    integer    rd_head, rd_tail, cycles;
    integer    beat;
    reg        rd_active;
    reg [31:0] rd_base;

    initial begin rd_head = 0; rd_tail = 0; beat = 0; rd_active = 0; cycles = 0; end

    always @(posedge clk) begin
        cycles <= cycles + 1;
        if (!resetn) begin
            rd_head = 0; rd_tail = 0; beat = 0; rd_active = 0;
            rvalid <= 0; rlast <= 0; arready <= 0;
        end else begin
            // address channel
            if (arvalid && arready) begin
                if (arlen != 3 || arsize != 3 || arburst != 1)
                    $fatal(1, "unexpected AXI read len=%0d size=%0d burst=%0d", arlen, arsize, arburst);
                rd_addr_q[rd_tail % 64] = araddr;
                rd_ready_cycle_q[rd_tail % 64] = cycles + latency_min + ($urandom % (latency_max - latency_min + 1));
                rd_tail = rd_tail + 1;
            end
            arready <= ((rd_tail - rd_head) < 60) && (($urandom % 100) >= ar_gap_pct);
            // data channel
            if (rvalid && rready) begin
                rvalid <= 0;
                rlast <= 0;
                if (beat == 3) begin
                    rd_head = rd_head + 1;
                    beat = 0;
                end else begin
                    beat = beat + 1;
                end
            end
            if ((!rvalid || rready) && rd_head != rd_tail && cycles >= rd_ready_cycle_q[rd_head % 64] &&
                (($urandom % 100) >= r_gap_pct)) begin
                rd_base = rd_addr_q[rd_head % 64] + beat * 8;
                rdata <= { texel_at_phys((rd_base >> 1) + 3), texel_at_phys((rd_base >> 1) + 2),
                           texel_at_phys((rd_base >> 1) + 1), texel_at_phys((rd_base >> 1) + 0) };
                rvalid <= 1;
                rlast <= (beat == 3);
            end
        end
    end

    // ------------------------------------------------------------ scoreboard
    reg [64:0] expq [$];
    integer checked, errors;
    integer m_stall_pct;

    always @(posedge clk) begin
        if (resetn) begin
            if (m_valid && m_ready) begin : check
                reg [64:0] e;
                if (expq.size() == 0) begin
                    $display("ERROR unexpected fragment output");
                    errors = errors + 1;
                end else begin
                    e = expq.pop_front();
                    if (t00 !== e[15:0] || (!e[64] && (t01 !== e[31:16] || t10 !== e[47:32] || t11 !== e[63:48]))) begin
                        $display("ERROR fragment %0d: got %h %h %h %h expected %h %h %h %h nearest=%0d",
                                 checked, t00, t01, t10, t11, e[15:0], e[31:16], e[47:32], e[63:48], e[64]);
                        errors = errors + 1;
                    end
                    checked = checked + 1;
                end
            end
            m_ready <= (($urandom % 100) >= m_stall_pct);
        end
    end

    // ----------------------------------------------------------------- driver
    integer gap_pct;

    task push;
        input [TEX_ADDR_WIDTH-1:0] p00, p01, p10, p11;
        reg [64:0] e;
        begin
            while (($urandom % 100) < gap_pct) begin
                s_valid <= 0;
                @(posedge clk);
            end
            a00 <= p00; a01 <= p01; a10 <= p10; a11 <= p11;
            e = { nearest, texel_at(p11), texel_at(p10), texel_at(p01), texel_at(p00) };
            expq.push_back(e);
            s_valid <= 1;
            @(posedge clk);
            while (!s_ready) @(posedge clk);
            s_valid <= 0;
        end
    endtask

    task drain;
        integer guard;
        begin
            s_valid <= 0;
            guard = 0;
            while ((expq.size() != 0) && guard < 2000000) begin
                @(posedge clk);
                guard = guard + 1;
            end
            if (expq.size() != 0) $fatal(1, "drain timeout, %0d fragments outstanding", expq.size());
            repeat (4) @(posedge clk);
        end
    endtask

    task new_page_table;
        integer i, j, tmp;
        integer perm [0:PAGES-1];
        begin
            drain;
            for (i = 0; i < PAGES; i = i + 1) perm[i] = i;
            for (i = PAGES - 1; i > 0; i = i - 1) begin
                j = $urandom % (i + 1);
                tmp = perm[i]; perm[i] = perm[j]; perm[j] = tmp;
            end
            region = region + 1;
            for (i = 0; i < PAGES; i = i + 1) page_phys[i] = region * 4096 + perm[i];
            for (i = 0; i < PAGES; i = i + 1) begin
                pt_valid <= 1;
                pt_last <= (i == PAGES - 1);
                pt_data <= page_phys[i] * PAGE_BYTES;
                @(posedge clk);
            end
            pt_valid <= 0;
            pt_last <= 0;
            repeat (3) @(posedge clk);
        end
    endtask

    function [TEX_ADDR_WIDTH-1:0] quad_addr;   // 256 texel wide texture
        input integer x, y;
        begin
            quad_addr = (y & 255) * 256 + (x & 255);
        end
    endfunction

    // one sequence of n fragments of the given pattern
    task run_sequence;
        input integer mode;
        input integer n;
        integer i, x, y, dx, dy, k;
        reg [TEX_ADDR_WIDTH-1:0] r0, r1, r2, r3;
        begin
            x = $urandom % 256; y = $urandom % 256;
            dx = 1; dy = 1;
            for (i = 0; i < n; i = i + 1) begin
                case (mode)
                    0: begin x = x + 1; end                                       // unit steps in s
                    1: begin y = y + 1; end                                       // unit steps in t
                    2: begin x = x + dx; y = y + dy; if ($urandom % 16 == 0) dx = -dx; if ($urandom % 16 == 0) dy = -dy; end
                    3: begin x = x + ($urandom % 5) - 2; y = y + ($urandom % 5) - 2; end
                    4: begin x = $urandom; y = $urandom; end
                    5: begin if (i % 2) x = x + 1; end                            // half steps
                    6: begin end                                                  // static
                    7: begin x = x - 1; end                                       // back steps in s
                    8: begin x = x + 1; if ($urandom % 4 == 0) y = y + 1; end     // gentle slope
                    9: begin x = x + ($urandom % 3) - 1; y = y + ($urandom % 3) - 1; end
                    default: begin end
                endcase
                case (mode)
                    10: begin   // arbitrary addresses
                        r0 = $urandom; r1 = $urandom; r2 = $urandom; r3 = $urandom;
                    end
                    15: begin   // arbitrary addresses inside the cache size (8 KiB): hits after warm-up
                        r0 = $urandom % 4096; r1 = $urandom % 4096; r2 = $urandom % 4096; r3 = $urandom % 4096;
                    end
                    11: begin   // addresses from a tiny set: many equal lanes
                        r0 = quad_addr(x + ($urandom % 2), y); r1 = quad_addr(x + ($urandom % 2), y);
                        r2 = quad_addr(x, y + ($urandom % 2)); r3 = quad_addr(x + ($urandom % 2), y + ($urandom % 2));
                    end
                    12: begin   // all lanes equal (1x1 texture)
                        r0 = quad_addr(x, y); r1 = r0; r2 = r0; r3 = r0;
                    end
                    13: begin   // clamped edge: the right and lower neighbours stay
                        r0 = quad_addr(x, y);
                        r1 = (x & 255) == 255 ? r0 : quad_addr(x + 1, y);
                        r2 = (y & 255) == 255 ? r0 : quad_addr(x, y + 1);
                        r3 = (((x & 255) == 255) ? r2 : quad_addr(x + 1, y + 1));
                    end
                    14: begin   // lanes taken from a shuffled quad: forwarding between all lanes
                        k = $urandom % 4;
                        case (k)
                            0: begin r0 = quad_addr(x + 1, y); r1 = quad_addr(x, y); r2 = quad_addr(x + 1, y + 1); r3 = quad_addr(x, y + 1); end
                            1: begin r0 = quad_addr(x, y + 1); r1 = quad_addr(x + 1, y + 1); r2 = quad_addr(x, y); r3 = quad_addr(x + 1, y); end
                            2: begin r0 = quad_addr(x + 1, y + 1); r1 = quad_addr(x, y + 1); r2 = quad_addr(x + 1, y); r3 = quad_addr(x, y); end
                            default: begin r0 = quad_addr(x, y); r1 = quad_addr(x + 1, y); r2 = quad_addr(x, y + 1); r3 = quad_addr(x + 1, y + 1); end
                        endcase
                    end
                    default: begin
                        r0 = quad_addr(x, y); r1 = quad_addr(x + 1, y);
                        r2 = quad_addr(x, y + 1); r3 = quad_addr(x + 1, y + 1);
                    end
                endcase
                push(r0, r1, r2, r3);
            end
        end
    endtask

    // ideal throughput run: no stalls, short fixed latency
    task perf;
        input integer mode;
        input integer n;
        integer c0;
        begin
            drain;
            gap_pct = 0; m_stall_pct = 0; ar_gap_pct = 0; r_gap_pct = 0; latency_min = 2; latency_max = 2;
            nearest = 0;
            c0 = cycles;
            run_sequence(mode, n);
            drain;
            $display("PERF mode=%0d fragments=%0d cycles=%0d cycles_per_fragment=%0d.%03d", mode, n, cycles - c0,
                     (cycles - c0) / n, (((cycles - c0) * 1000) / n) % 1000);
        end
    endtask

    integer sequences, fragments, s, m, perf_only;
    initial begin
        errors = 0; checked = 0; region = 0;
        if (!$value$plusargs("SEED=%d", seed)) seed = 1;
        if (!$value$plusargs("SEQUENCES=%d", sequences)) sequences = 60;
        if (!$value$plusargs("FRAGMENTS=%d", fragments)) fragments = 400;
        perf_only = $test$plusargs("PERF");
        s = $urandom(seed);
        gap_pct = 0; m_stall_pct = 0; ar_gap_pct = 0; r_gap_pct = 0; latency_min = 2; latency_max = 2;
        repeat (4) @(posedge clk);
        resetn <= 1;
        repeat (4) @(posedge clk);
        new_page_table;
        // wait for the initial cache invalidation sweep to finish
        repeat (300) @(posedge clk);
        if (perf_only) begin
            for (m = 0; m < 16; m = m + 1) perf(m, 20000);
        end else begin
            for (s = 0; s < sequences; s = s + 1) begin
                m = $urandom % 16;
                nearest = ($urandom % 4) == 0;
                gap_pct = ($urandom % 3 == 0) ? 0 : ($urandom % 50);
                m_stall_pct = ($urandom % 3 == 0) ? 0 : ($urandom % 70);
                ar_gap_pct = $urandom % 60;
                r_gap_pct = $urandom % 60;
                latency_min = 2 + $urandom % 4;
                latency_max = latency_min + $urandom % 24;
                if ($urandom % 6 == 0) new_page_table;
                run_sequence(m, 1 + ($urandom % fragments));
                drain;
            end
        end
        drain;
        $display("TEXTURE_READER checked=%0d errors=%0d cycles=%0d", checked, errors, cycles);
        if (errors != 0) $fatal(1, "texture reader mismatches: %0d", errors);
        $display("PASS texture reader");
        $finish;
    end

    initial begin
        #2000000000;
        $fatal(1, "global timeout");
    end
endmodule
