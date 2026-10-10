`timescale 1ns/1ps
/*
 * Receive-window read-ahead in mntzorro.v, driven through the Zorro III pins
 * against a modelled AXI read slave.  Autoconfig is skipped: the testbench
 * writes the configured state into the DUT once and lets it run.
 *
 * Every read's data is checked against the DDR model; the AXI read requests
 * are counted and their length checked.  Prints PASS/FAIL per case and a
 * final verdict.
 */
module rxpf_tb;
  localparam BOARD   = 32'h48000000;
  localparam BACKLOG = 32'h3fe00000;

  reg clk = 0;
  always #5 clk = ~clk;               // 100 MHz S_AXI_ACLK / m00_axi_aclk

  // Zorro pins
  reg  [22:0] za_drv;  reg za_oe = 0;
  reg  [15:0] zd_drv;  reg zd_oe = 0;
  wire [22:0] ZORRO_ADDR = za_oe ? za_drv : 23'bz;
  wire [15:0] ZORRO_DATA = zd_oe ? zd_drv : 16'bz;
  reg ZORRO_READ = 1, ZORRO_NUDS = 1, ZORRO_NLDS = 1, ZORRO_NDS1 = 1, ZORRO_NDS0 = 1;
  reg ZORRO_NCCS = 1, ZORRO_NFCS = 1, ZORRO_DOE = 0, ZORRO_NIORST = 1, ZORRO_NCFGIN = 1;
  wire ZORRO_NDTACK;

  // AXI read slave model
  wire [31:0] araddr; wire [3:0] arlen; wire [1:0] arburst; wire arvalid;
  reg  arready = 1;
  reg  [31:0] rdata = 0; reg rvalid = 0; reg rlast = 0;
  integer ar_count = 0;
  reg [31:0] gen = 0;                  // bumps model data: "the ARM rewrote DDR"

  function [31:0] model(input [31:0] a, input [31:0] g);
    model = (a * 32'h9e3779b1) ^ (g * 32'h85ebca6b) ^ 32'h5a5a0000;
  endfunction

  // a queue of outstanding bursts, served in order after a fixed latency
  reg [31:0] q_addr [0:15]; reg [4:0] q_len [0:15]; reg [31:0] q_gen [0:15];
  integer q_head = 0, q_tail = 0, beat = 0, wait_cnt = 0;
  // +RANDOM_ARREADY: the interconnect holds arready low at random
  reg random_arready = 0;
  initial random_arready = $test$plusargs("RANDOM_ARREADY");
  // +RAGGED: 5..68 cycle latency per burst and random gaps between beats
  reg ragged = 0;
  integer lat = 20;
  initial ragged = $test$plusargs("RAGGED");
  always @(posedge clk) if (random_arready) arready <= $random;
  // late_arready: arready follows arvalid one cycle late, as an interconnect
  // that is still waking up -- the shape that exposes a handshake that moves
  // on before arvalid is dropped.  drop_next: deliver one beat of the next
  // burst and lose the rest with no rlast, as an interconnect reset would.
  reg late_arready = 0, drop_next = 0;
  always @(posedge clk) if (late_arready) arready <= arvalid;
  // aresetn: the fabric reset.  stall_next: after the first beat of the next
  // multi-beat burst, deliver nothing for stall_cycles, then the rest (a slow
  // but legal slave).  hold_ar: arready held low.  rresp_err: SLVERR on beats.
  reg aresetn = 1, stall_next = 0, hold_ar = 0, rresp_err = 0;
  integer stall_cycles = 2000, stall_left = 0;
  always @(posedge clk) if (hold_ar) arready <= 0;
  reg [31:0] last_arlen, last_araddr;
  always @(posedge clk) begin
    if (arvalid && arready) begin
      q_addr[q_tail % 16] <= araddr; q_len[q_tail % 16] <= arlen + 1;
      q_gen[q_tail % 16] <= gen; q_tail <= q_tail + 1;
      ar_count <= ar_count + 1; last_arlen <= arlen; last_araddr <= araddr;
      if (arlen != 0 && arburst != 2'b01) begin
        $display("FAIL burst with arburst=%0d", arburst); errors = errors + 1;
      end
    end
    rvalid <= 0; rlast <= 0;
    if (!aresetn) begin
      q_head <= q_tail; beat <= 0; wait_cnt <= 0; stall_left <= 0;   // the interconnect forgets everything
    end else if (stall_left > 0) begin
      stall_left <= stall_left - 1;
    end else if (q_head != q_tail) begin
      if (wait_cnt < lat) wait_cnt <= wait_cnt + 1;  // ~200 ns DDR latency by default
      else if (ragged && beat != 0 && ($random & 3) == 0) begin
        // a cycle with no beat inside the burst
      end else begin
        rvalid <= 1;
        rdata  <= model(q_addr[q_head % 16] + beat * 4, q_gen[q_head % 16]);
        if (stall_next && q_len[q_head % 16] > 1 && beat == 0) begin
          stall_next <= 0; stall_left <= stall_cycles;
        end
        if (drop_next && q_len[q_head % 16] > 1) begin
          // one beat, then the rest of this burst and everything queued is gone
          drop_next <= 0; q_head <= q_tail; beat <= 0; wait_cnt <= 0;
        end else
        if (beat + 1 == q_len[q_head % 16]) begin
          rlast <= 1; beat <= 0; q_head <= q_head + 1; wait_cnt <= 0;
          if (ragged) lat <= 5 + ($random & 63);
        end else beat <= beat + 1;
      end
    end
  end

  integer errors = 0, hung = 0, hang = 0;
  reg first_req_seen = 0, aresetn_d = 1, zorro_started = 0;
  integer valid_in_reset = 0, early_valid = 0;
  always @(posedge clk) begin
    if (arvalid === 1'b1) first_req_seen <= 1;
    // from configuration until the first Zorro cycle starts, ARVALID is 0 (never X)
    if (!zorro_started && arvalid !== 1'b0) begin
      early_valid = early_valid + 1;
      $display("INFO ARVALID=%b at %t before the first request (zorro_state %0d)", arvalid, $time, dut.zorro_state);
    end
    // synchronous reset: ARVALID drops on the first edge with aresetn low;
    // from the second such edge on it must be low
    if (!aresetn && aresetn_d == 1'b0 && arvalid !== 1'b0) valid_in_reset = valid_in_reset + 1;
    aresetn_d <= aresetn;
  end

  MNTZorro_v0_1_S00_AXI dut (
    .ZORRO_ADDR(ZORRO_ADDR), .ZORRO_DATA(ZORRO_DATA),
    .ZORRO_NBGN(1'b1), .ZORRO_READ(ZORRO_READ), .ZORRO_NUDS(ZORRO_NUDS), .ZORRO_NLDS(ZORRO_NLDS),
    .ZORRO_NDS1(ZORRO_NDS1), .ZORRO_NDS0(ZORRO_NDS0), .ZORRO_NCCS(ZORRO_NCCS), .ZORRO_NFCS(ZORRO_NFCS),
    .ZORRO_DOE(ZORRO_DOE), .ZORRO_NIORST(ZORRO_NIORST), .ZORRO_NCFGIN(ZORRO_NCFGIN),
    .ZORRO_E7M(1'b0), .ZORRO_C28D(1'b0),
    .VCAP_VSYNC(1'b0), .VCAP_HSYNC(1'b0),
    .VCAP_G0(1'b0), .VCAP_G1(1'b0), .VCAP_G2(1'b0), .VCAP_G3(1'b0), .VCAP_G4(1'b0), .VCAP_G5(1'b0), .VCAP_G6(1'b0), .VCAP_G7(1'b0),
    .VCAP_B0(1'b0), .VCAP_B1(1'b0), .VCAP_B2(1'b0), .VCAP_B3(1'b0), .VCAP_B4(1'b0), .VCAP_B5(1'b0), .VCAP_B6(1'b0), .VCAP_B7(1'b0),
    .VCAP_R0(1'b0), .VCAP_R1(1'b0), .VCAP_R2(1'b0), .VCAP_R3(1'b0), .VCAP_R4(1'b0), .VCAP_R5(1'b0), .VCAP_R6(1'b0), .VCAP_R7(1'b0),
    .ZORRO_NDTACK(ZORRO_NDTACK),
    .m00_axi_aclk(clk), .m00_axi_aresetn(aresetn),
    .m00_axi_awready(1'b1), .m00_axi_wready(1'b1), .m00_axi_bresp(2'b00), .m00_axi_bvalid(1'b0),
    .m00_axi_arready(arready), .m00_axi_araddr(araddr), .m00_axi_arlen(arlen), .m00_axi_arburst(arburst),
    .m00_axi_arvalid(arvalid), .m00_axi_rdata(rdata), .m00_axi_rresp(rresp_err ? 2'b10 : 2'b00), .m00_axi_rlast(rlast), .m00_axi_rvalid(rvalid),
    .m01_axi_aclk(clk), .m01_axi_aresetn(1'b1), .m01_axi_awready(1'b1), .m01_axi_wready(1'b1),
    .m01_axi_bresp(2'b00), .m01_axi_bvalid(1'b0),
    .video_control_vblank_in(2'b00), .source_sync_diagnostic(64'd0),
    .S_AXI_ACLK(clk), .S_AXI_ARESETN(1'b1),
    .S_AXI_AWADDR(0), .S_AXI_AWPROT(3'b0), .S_AXI_AWVALID(1'b0), .S_AXI_WDATA(0), .S_AXI_WSTRB(0),
    .S_AXI_WVALID(1'b0), .S_AXI_BREADY(1'b1), .S_AXI_ARADDR(0), .S_AXI_ARPROT(3'b0),
    .S_AXI_ARVALID(1'b0), .S_AXI_RREADY(1'b1)
  );

  // one Zorro III longword (or word) cycle at absolute address a
  integer lat_fcs, lat_ds, t_fcs, t_ds;
  task z3cycle(input [31:0] a, input rd, input [1:0] lanes /*3=long,2=hi word,1=lo word*/,
               output [31:0] got, output integer cycles_ns);
    integer t0;
    begin
      t0 = $time;
      zorro_started = 1;
      za_drv = {a[23:2], 1'b0}; zd_drv = {a[31:24], 8'h00};
      za_oe = 1; zd_oe = 1; ZORRO_READ = rd;
      #30 ZORRO_NFCS = 0; t_fcs = $time;
      #40 za_oe = 0; zd_oe = !rd;
      if (!rd) begin zd_drv = 16'hdead; za_drv = {16'hbeef, 7'h0}; za_oe = 1; end
      ZORRO_DOE = 1;
      #10 ZORRO_NUDS = !lanes[1]; ZORRO_NLDS = !lanes[1]; ZORRO_NDS1 = !lanes[0]; ZORRO_NDS0 = !lanes[0]; t_ds = $time;
      for (hang = 0; hang < 30000 && ZORRO_NDTACK !== 1'b1; hang = hang + 1) #1;
      lat_fcs = $time - t_fcs; lat_ds = $time - t_ds;
      if (ZORRO_NDTACK !== 1'b1) begin hung = hung + 1; $display("INFO z3 cycle at %h got no DTACK in 30 us (state %0d busy %0d in_ram %0d fcs %0d)", a, dut.zorro_state, dut.rd_out, dut.z3addr_in_ram, dut.z3_fcs_state); end
      #20;
      got = {dut.data_z3_hi16, dut.data_z3_low16};
      ZORRO_NUDS = 1; ZORRO_NLDS = 1; ZORRO_NDS1 = 1; ZORRO_NDS0 = 1;
      ZORRO_DOE = 0; za_oe = 0; zd_oe = 0;
      #10 ZORRO_NFCS = 1;
      for (hang = 0; hang < 3000 && ZORRO_NDTACK !== 1'b0; hang = hang + 1) #10;
      #60;
      cycles_ns = $time - t0;
    end
  endtask

  function [31:0] swapped(input [31:0] r);   // what data_z3_hi16/low16 hold for an AXI word
    swapped = {r[7:0], r[15:8], r[23:16], r[31:24]};
  endfunction

  function [31:0] ddr(input [31:0] off, input [20:0] sel);
    ddr = (BACKLOG - 32'h2000) + off + {sel, 11'h0};
  endfunction

  task expect_read(input [31:0] off, input [20:0] sel, input [1:0] lanes, input [31:0] g, input [127:0] what);
    reg [31:0] got, want; integer ns;
    begin
      z3cycle(BOARD + off, 1, lanes, got, ns);
      want = swapped(model(ddr(off, sel) & 32'hfffffffc, g));
      if (got !== want) begin
        $display("FAIL %0s off=%h got=%h want=%h", what, off, got, want); errors = errors + 1;
      end
    end
  endtask

  integer i, n0, ns, k; reg [31:0] got;
  initial begin
    $timeformat(-9, 0, " ns", 8);
    if ($test$plusargs("BOOT")) begin : boot_arm
      integer n0b;
      repeat (20) @(posedge clk);
      dut.z3_ram_low = BOARD; dut.z3_confdone = 1; dut.slv_reg4 = 2;
      dut.zorro_state = 9; repeat (4) @(posedge clk); dut.zorro_state = 12; repeat (10) @(posedge clk);
      // B1: a card-memory read while arready lags arvalid: exactly one request
      late_arready = 1; arready = 0;
      n0b = ar_count;
      z3cycle(BOARD + 32'h20000, 1, 2'b11, got, ns);
      repeat (200) @(posedge clk);
      $display("%s late arready, non-window read: %0d request(s) (want 1)", (ar_count - n0b == 1) ? "PASS" : "FAIL", ar_count - n0b);
      if (ar_count - n0b != 1) errors = errors + 1;
      n0b = ar_count;
      z3cycle(BOARD + 32'h2000, 1, 2'b11, got, ns);
      repeat (200) @(posedge clk);
      $display("%s late arready, window read: %0d request(s) (want 1)", (ar_count - n0b == 1) ? "PASS" : "FAIL", ar_count - n0b);
      if (ar_count - n0b != 1) errors = errors + 1;
      late_arready = 0; @(posedge clk); #1 arready = 1; @(posedge clk);
      // B2: a burst loses its beats because the fabric resets under it
      //     (aresetn), during a read that wants a later beat.  The read must
      //     come back with the right data, and so must the ones after it.
      dut.slv_reg4 = 5; repeat (4) @(posedge clk);
      expect_read(32'h2000, 5, 2'b11, gen, "B2-a");
      stall_next = 1;                        // the next fill stalls after one beat ...
      dut.slv_reg4 = 6; repeat (4) @(posedge clk);
      hung = 0;
      fork
        expect_read(32'h2000, 6, 2'b11, gen, "B2-b");   // gets beat 0
        begin repeat (300) @(posedge clk); aresetn = 0; repeat (16) @(posedge clk); aresetn = 1; end
      join
      expect_read(32'h2004, 6, 2'b11, gen, "B2-c");     // the fill was lost: asks again
      expect_read(32'h2008, 6, 2'b11, gen, "B2-d");
      z3cycle(BOARD + 32'h20000, 1, 2'b11, got, ns);
      $display("%s fabric reset mid-burst: data checked, %0d hung", hung == 0 ? "PASS" : "FAIL", hung);
      if (hung != 0) errors = errors + 1;

      // B3: a legal but slow slave: 2000 cycles between beat 0 and beat 1.
      //     Nothing may give the burst up; every word must still be right,
      //     and the read after it must get its own data, not the old tail.
      dut.slv_reg4 = 7; repeat (4) @(posedge clk);
      stall_next = 1; stall_cycles = 2000;
      hung = 0;
      for (i = 0; i < 16; i = i + 1) expect_read(32'h2000 + i*4, 7, 2'b11, gen, "B3-seq");
      expect_read(32'h2100, 7, 2'b11, gen, "B3-next");
      $display("%s slow slave (20 us stall inside a burst): data checked, %0d hung", hung == 0 ? "PASS" : "FAIL", hung);
      if (hung != 0) errors = errors + 1;

      // B3b: the same stall, but the next read is OUTSIDE the window, so a
      //      design that gave the burst up would issue it while the old tail
      //      is still owed, and take an old beat as its data.
      dut.slv_reg4 = 12; repeat (4) @(posedge clk);
      stall_next = 1; stall_cycles = 2000;
      hung = 0;
      expect_read(32'h2000, 12, 2'b11, gen, "B3b-a");
      z3cycle(BOARD + 32'h20000, 1, 2'b11, got, ns);
      if (got !== swapped(model(last_araddr, gen))) begin
        $display("FAIL B3b non-window read after a stalled fill: got %h want %h (req %h)", got, swapped(model(last_araddr, gen)), last_araddr);
        errors = errors + 1;
      end else $display("PASS non-window read while an old fill's tail is still owed: data checked, %0d hung", hung);
      if (hung != 0) errors = errors + 1;

      // B4: arready held low, a read requests, the Amiga resets.  The request
      //     is accepted after the reset; its beats belong to nobody and must
      //     be drained and dropped.  The reads after must get their own data.
      dut.slv_reg4 = 8; repeat (4) @(posedge clk);
      hold_ar = 1; arready = 0;
      hung = 0;
      z3cycle(BOARD + 32'h2000, 1, 2'b11, got, ns);      // no handshake: no DTACK
      ZORRO_NIORST = 0; repeat (20) @(posedge clk); ZORRO_NIORST = 1; repeat (20) @(posedge clk);
      hold_ar = 0; @(posedge clk); #1 arready = 1; repeat (100) @(posedge clk);
      dut.z3_ram_low = BOARD; dut.z3_confdone = 1;
      dut.zorro_state = 9; repeat (4) @(posedge clk); dut.zorro_state = 12; repeat (10) @(posedge clk);
      hung = 0;
      gen = gen + 1;
      expect_read(32'h2040, 8, 2'b11, gen, "B4-a");
      expect_read(32'h2044, 8, 2'b11, gen, "B4-b");
      z3cycle(BOARD + 32'h20000, 1, 2'b11, got, ns);
      $display("%s request pending across a Zorro reset: data checked, %0d hung", hung == 0 ? "PASS" : "FAIL", hung);
      if (hung != 0) errors = errors + 1;

      // B5: the presented slot changes while a fill is still arriving.
      dut.slv_reg4 = 10; repeat (4) @(posedge clk);
      stall_next = 1; stall_cycles = 300;
      expect_read(32'h2000, 10, 2'b11, gen, "B5-a");
      dut.slv_reg4 = 11; repeat (4) @(posedge clk);
      expect_read(32'h2004, 11, 2'b11, gen, "B5-b");
      expect_read(32'h2008, 11, 2'b11, gen, "B5-c");
      $display("PASS slot change during a fill (data checked above)");

      // B4b: Zorro reset in the middle of a burst (accepted, beats still owed):
      //      ownership kept, the tail drained and dropped, one new request after.
      dut.slv_reg4 = 13; repeat (4) @(posedge clk);
      stall_next = 1; stall_cycles = 600;
      expect_read(32'h2000, 13, 2'b11, gen, "B4b-a");       // beat 0, then the stall
      ZORRO_NIORST = 0; repeat (20) @(posedge clk); ZORRO_NIORST = 1; repeat (20) @(posedge clk);
      dut.z3_ram_low = BOARD; dut.z3_confdone = 1;
      dut.zorro_state = 9; repeat (4) @(posedge clk); dut.zorro_state = 12; repeat (10) @(posedge clk);
      n0 = ar_count; hung = 0;
      expect_read(32'h2080, 13, 2'b11, gen, "B4b-b");       // waits for the old tail, then its own fill
      $display("%s Zorro reset mid-burst: %0d new request(s) (want 1), %0d hung, data checked",
               (ar_count - n0 == 1 && hung == 0) ? "PASS" : "FAIL", ar_count - n0, hung);
      if (!(ar_count - n0 == 1 && hung == 0)) errors = errors + 1;

      // B2b: fabric reset while a request is PENDING (ARREADY low): ARVALID must
      //      drop and stay low through the reset, and the read is asked again
      //      exactly once after release.
      dut.slv_reg4 = 14; repeat (4) @(posedge clk);
      hold_ar = 1; arready = 0;
      n0 = ar_count; hung = 0;
      fork
        expect_read(32'h2000, 14, 2'b11, gen, "B2b");
        begin
          repeat (200) @(posedge clk); aresetn = 0; repeat (16) @(posedge clk);
          hold_ar = 0; @(posedge clk); #1 arready = 1; repeat (4) @(posedge clk); aresetn = 1;
        end
      join
      $display("%s fabric reset with a request pending: %0d handshake(s) (want 1), %0d hung, data checked",
               (ar_count - n0 == 1 && hung == 0) ? "PASS" : "FAIL", ar_count - n0, hung);
      if (!(ar_count - n0 == 1 && hung == 0)) errors = errors + 1;

      // B7 (codex 00:43): Zorro RESET while ARVALID is held by ARREADY=0,
      //     with the fabric reset pulsed for a single sampled edge during it.
      //     Afterwards the port must be fully idle: the next window fill and a
      //     non-window read return their own data, nothing hangs.
      dut.slv_reg4 = 15; repeat (4) @(posedge clk);
      hold_ar = 1; arready = 0;
      hung = 0;
      fork
        z3cycle(BOARD + 32'h2000, 1, 2'b11, got, ns);   // requests; no handshake
        begin
          repeat (100) @(posedge clk);
          ZORRO_NIORST = 0;
          repeat (10) @(posedge clk);
          @(negedge clk) aresetn = 0; @(negedge clk) aresetn = 1;   // one sampled edge
          repeat (10) @(posedge clk);
          ZORRO_NIORST = 1;
        end
      join
      hold_ar = 0; @(posedge clk); #1 arready = 1; repeat (20) @(posedge clk);
      dut.z3_ram_low = BOARD; dut.z3_confdone = 1;
      dut.zorro_state = 9; repeat (4) @(posedge clk); dut.zorro_state = 12; repeat (10) @(posedge clk);
      $display("INFO B7 port after resets: arvalid=%b rd_out=%b rd_discard=%b rxpf_valid=%b", arvalid, dut.rd_out, dut.rd_discard, dut.rxpf_valid);
      hung = 0;
      gen = gen + 1;
      expect_read(32'h2400, 15, 2'b11, gen, "B7-a");
      expect_read(32'h2404, 15, 2'b11, gen, "B7-b");
      z3cycle(BOARD + 32'h20000, 1, 2'b11, got, ns);
      if (got !== swapped(model(last_araddr, gen))) begin
        $display("FAIL B7 non-window read: got %h want %h", got, swapped(model(last_araddr, gen))); errors = errors + 1;
      end
      $display("%s Zorro reset + 1-edge fabric reset with a pending request: data checked, %0d hung", hung == 0 ? "PASS" : "FAIL", hung);
      if (hung != 0) errors = errors + 1;

      // B6: SLVERR responses do not wedge the port.
      dut.slv_reg4 = 11; repeat (4) @(posedge clk);
      rresp_err = 1; hung = 0;
      z3cycle(BOARD + 32'h2000, 1, 2'b11, got, ns);
      z3cycle(BOARD + 32'h20000, 1, 2'b11, got, ns);
      rresp_err = 0;
      expect_read(32'h2100, 11, 2'b11, gen, "B6-after");
      $display("%s SLVERR beats: %0d hung", hung == 0 ? "PASS" : "FAIL", hung);
      if (hung != 0) errors = errors + 1;
      $display("%s ARVALID at startup before the first request: %0d cycle(s) not 0 (want 0)", early_valid == 0 ? "PASS" : "FAIL", early_valid);
      if (early_valid != 0) errors = errors + 1;
      $display("%s ARVALID while aresetn low: %0d cycle(s) (want 0)", valid_in_reset == 0 ? "PASS" : "FAIL", valid_in_reset);
      if (valid_in_reset != 0) errors = errors + 1;
      $display("%s rxpf_tb: %0d error(s)", errors == 0 ? "PASS" : "FAIL", errors);
      if (errors == 0) $display("RXPF_VERDICT_OK");
      $finish;
    end
    repeat (20) @(posedge clk);
    // configured at BOARD, slot 3
    dut.z3_ram_low = BOARD; dut.z3_confdone = 1; dut.slv_reg4 = 3;
    dut.zorro_state = 9;    // CONFIGURED: derives the window bounds
    repeat (4) @(posedge clk);
    dut.zorro_state = 12;   // Z3_IDLE
    repeat (10) @(posedge clk);
    $display("INFO configured: ram %h-%h reg %h-%h", dut.z3_ram_low, dut.z3_ram_high, dut.z3_reg_low, dut.z3_reg_high);

    // 1. a 1500-byte frame read front to back: one burst per 64 bytes
    n0 = ar_count;
    begin : latstat
      integer mn, mx, sum, mnd, mxd, k;
      mn = 99999; mx = 0; sum = 0; mnd = 99999; mxd = 0;
      for (i = 0; i < 375; i = i + 1) begin
        k = ar_count;
        expect_read(32'h2000 + i*4, 3, 2'b11, gen, "seq");
        if (ar_count == k) begin  // a read-ahead hit
          if (lat_fcs < mn) mn = lat_fcs; if (lat_fcs > mx) mx = lat_fcs; sum = sum + lat_fcs;
          if (lat_ds < mnd) mnd = lat_ds; if (lat_ds > mxd) mxd = lat_ds;
        end
      end
      $display("INFO latency hit FCS->DTACK min %0d max %0d ns; DS->DTACK min %0d max %0d ns", mn, mx, mnd, mxd);
    end
    $display("%s seq: %0d longs, %0d AXI requests (want 24)", (ar_count - n0 == 24) ? "PASS" : "FAIL",
             375, ar_count - n0);
    if (ar_count - n0 != 24) errors = errors + 1;

    // 2. header poll: the same address twice sees DDR change in between
    expect_read(32'h2000, 3, 2'b11, gen, "poll-a");
    gen = gen + 1;
    n0 = ar_count;
    expect_read(32'h2000, 3, 2'b11, gen, "poll-b");
    $display("%s poll re-read goes to DDR (%0d request)", (ar_count - n0 == 1) ? "PASS" : "FAIL", ar_count - n0);
    if (ar_count - n0 != 1) errors = errors + 1;

    // 3. a write retires the line
    expect_read(32'h2100, 3, 2'b11, gen, "w-a");
    z3cycle(BOARD + 32'h9000, 0, 2'b11, got, ns);          // TX window write
    gen = gen + 1;
    n0 = ar_count;
    expect_read(32'h2104, 3, 2'b11, gen, "w-b");
    $display("%s write retires the line (%0d request)", (ar_count - n0 == 1) ? "PASS" : "FAIL", ar_count - n0);
    if (ar_count - n0 != 1) errors = errors + 1;

    // 4. a new presented slot retires the line
    expect_read(32'h2200, 3, 2'b11, gen, "s-a");
    dut.slv_reg4 = 9;
    repeat (4) @(posedge clk);
    n0 = ar_count;
    expect_read(32'h2204, 9, 2'b11, gen, "s-b");
    $display("%s slot change retires the line (%0d request)", (ar_count - n0 == 1) ? "PASS" : "FAIL", ar_count - n0);
    if (ar_count - n0 != 1) errors = errors + 1;

    // 5. word reads.  Zorro III puts the same longword address on the bus for
    //    A and A+2 (only the lane strobes differ), so the second word is not
    //    "forward" and goes back to DDR, as every read did before; A+4 hits.
    n0 = ar_count;
    expect_read(32'h2300, 9, 2'b10, gen, "wd-a");
    expect_read(32'h2302, 9, 2'b01, gen, "wd-b");
    expect_read(32'h2304, 9, 2'b10, gen, "wd-c");
    $display("%s word reads: %0d requests (want 2)", (ar_count - n0 == 2) ? "PASS" : "FAIL", ar_count - n0);
    if (ar_count - n0 != 2) errors = errors + 1;

    // 6. 4 KB boundary: slot 1 + 0x7d8 ends 40 bytes before a 4 KB line
    dut.slv_reg4 = 1; repeat (4) @(posedge clk);
    n0 = ar_count;
    expect_read(32'h27d8, 1, 2'b11, gen, "4k");
    $display("%s burst near 4 KB boundary: arlen=%0d (want 9)", (last_arlen == 9) ? "PASS" : "FAIL", last_arlen);
    if (last_arlen != 9) errors = errors + 1;
    for (k = 1; k < 10; k = k + 1) expect_read(32'h27d8 + k*4, 1, 2'b11, gen, "4k-seq");
    if (ar_count - n0 != 1) begin $display("FAIL 4k line refetched"); errors = errors + 1; end

    // 7. outside the window: single beats, unchanged
    n0 = ar_count;
    z3cycle(BOARD + 32'h20000, 1, 2'b11, got, ns);
    $display("%s non-window read single beat: arlen=%0d", (last_arlen == 0 && ar_count - n0 == 1) ? "PASS" : "FAIL", last_arlen);
    if (!(last_arlen == 0 && ar_count - n0 == 1)) errors = errors + 1;

    // 8. timing: a hit versus a miss, in Zorro cycle length
    dut.slv_reg4 = 4; repeat (4) @(posedge clk);
    z3cycle(BOARD + 32'h2000, 1, 2'b11, got, ns); $display("INFO miss cycle %0d ns", ns);
    z3cycle(BOARD + 32'h2004, 1, 2'b11, got, ns); $display("INFO hit  cycle %0d ns", ns);

    $display("%s rxpf_tb: %0d error(s)", errors == 0 ? "PASS" : "FAIL", errors);
    if (errors == 0) $display("RXPF_VERDICT_OK");
    $finish;
  end

  initial begin #20000000 $display("FAIL timeout"); $finish; end
endmodule
