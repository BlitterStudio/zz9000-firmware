`timescale 1ns/1ps
/*
 * Zorro III read answers through production MNTZorro: DTACK must not lead
 * the data. A read state that loads data_z3_* and sets DTACK on the same
 * edge puts DTACK on the bus before its data, because the data pins follow
 * data_z3_* at the earliest after the load edge.
 *
 * A monitor samples the pins on every clock edge, before the edge's own
 * updates. At the edge where ZORRO_NDTACK starts to assert, the pins must
 * already carry the value the cycle answers with; a second look at the pins
 * 20 ns after DTACK checks that the value stays. Four read paths are driven:
 *   axi   card memory read through m00 (single beat)
 *   rxpf  receive-window read-ahead, hits and misses along a few bursts
 *   reg   register read
 *   arm   read answered by the ARM, with a slow acknowledge and with the
 *         shortest one the handshake allows (the flag high for one clock)
 * The AXI slave and the ARM handshake flag are modelled here; the register
 * file and AXI-Lite side are not involved.
 */
module dtack_order_tb;
  localparam [31:0] BOARD = 32'h48000000;

  reg clk = 0;
  always #5 clk = ~clk;

  reg [22:0] za_drv = 0; reg za_oe = 0;
  reg [15:0] zd_drv = 0; reg zd_oe = 0;
  wire [22:0] ZORRO_ADDR = za_oe ? za_drv : 23'bz;
  wire [15:0] ZORRO_DATA = zd_oe ? zd_drv : 16'bz;
  reg ZORRO_READ = 1, ZORRO_NUDS = 1, ZORRO_NLDS = 1;
  reg ZORRO_NDS1 = 1, ZORRO_NDS0 = 1, ZORRO_NCCS = 1, ZORRO_NFCS = 1;
  reg ZORRO_DOE = 0, ZORRO_NIORST = 1, ZORRO_NCFGIN = 1;
  wire ZORRO_NDTACK;

  wire [31:0] araddr; wire [3:0] arlen; wire arvalid;
  reg arready = 1;
  reg [31:0] rdata = 0;
  reg rvalid = 0, rlast = 0;

  MNTZorro_v0_1_S00_AXI dut (
    .ZORRO_ADDR(ZORRO_ADDR), .ZORRO_DATA(ZORRO_DATA),
    .ZORRO_NBGN(1'b1), .ZORRO_READ(ZORRO_READ), .ZORRO_NUDS(ZORRO_NUDS),
    .ZORRO_NLDS(ZORRO_NLDS), .ZORRO_NDS1(ZORRO_NDS1), .ZORRO_NDS0(ZORRO_NDS0),
    .ZORRO_NCCS(ZORRO_NCCS), .ZORRO_NFCS(ZORRO_NFCS), .ZORRO_DOE(ZORRO_DOE),
    .ZORRO_NIORST(ZORRO_NIORST), .ZORRO_NCFGIN(ZORRO_NCFGIN), .ZORRO_E7M(1'b0),
    .ZORRO_C28D(1'b0), .VCAP_VSYNC(1'b0), .VCAP_HSYNC(1'b0),
    .VCAP_G0(1'b0), .VCAP_G1(1'b0), .VCAP_G2(1'b0), .VCAP_G3(1'b0),
    .VCAP_G4(1'b0), .VCAP_G5(1'b0), .VCAP_G6(1'b0), .VCAP_G7(1'b0),
    .VCAP_B0(1'b0), .VCAP_B1(1'b0), .VCAP_B2(1'b0), .VCAP_B3(1'b0),
    .VCAP_B4(1'b0), .VCAP_B5(1'b0), .VCAP_B6(1'b0), .VCAP_B7(1'b0),
    .VCAP_R0(1'b0), .VCAP_R1(1'b0), .VCAP_R2(1'b0), .VCAP_R3(1'b0),
    .VCAP_R4(1'b0), .VCAP_R5(1'b0), .VCAP_R6(1'b0), .VCAP_R7(1'b0),
    .ZORRO_NDTACK(ZORRO_NDTACK),
    .m00_axi_aclk(clk), .m00_axi_aresetn(1'b1),
    .m00_axi_awready(1'b1), .m00_axi_wready(1'b1),
    .m00_axi_bresp(2'b00), .m00_axi_bvalid(1'b0),
    .m00_axi_arready(arready), .m00_axi_araddr(araddr), .m00_axi_arlen(arlen),
    .m00_axi_arvalid(arvalid),
    .m00_axi_rdata(rdata), .m00_axi_rresp(2'b00), .m00_axi_rlast(rlast), .m00_axi_rvalid(rvalid),
    .m01_axi_aclk(clk), .m01_axi_aresetn(1'b1), .m01_axi_awready(1'b1),
    .m01_axi_wready(1'b1), .m01_axi_bresp(2'b00), .m01_axi_bvalid(1'b0),
    .video_control_vblank_in(2'b00), .source_sync_diagnostic(64'd0),
    .S_AXI_ACLK(clk), .S_AXI_ARESETN(1'b1), .S_AXI_AWADDR(0), .S_AXI_AWPROT(3'b0),
    .S_AXI_AWVALID(1'b0), .S_AXI_WDATA(0), .S_AXI_WSTRB(0), .S_AXI_WVALID(1'b0),
    .S_AXI_BREADY(1'b1), .S_AXI_ARADDR(0), .S_AXI_ARPROT(3'b0),
    .S_AXI_ARVALID(1'b0), .S_AXI_RREADY(1'b1)
  );

  function [31:0] mem(input [31:0] a);
    mem = ((a & 32'hfffffffc) * 32'h9e3779b1) ^ 32'h5a5a1234;
  endfunction

  // what data_z3_hi16/low16 hold for an AXI word: all four bytes reversed
  function [31:0] swapped(input [31:0] r);
    swapped = {r[7:0], r[15:8], r[23:16], r[31:24]};
  endfunction

  // -- AXI read slave: arready always high, bursts after a fixed latency -------
  reg        r_busy = 0;
  reg [31:0] r_base = 0;
  integer    r_len = 0, r_beat = 0, r_wait = 0;
  always @(posedge clk) begin
    rvalid <= 0;
    rlast  <= 0;
    if (arvalid && arready && !r_busy) begin
      r_busy <= 1; r_base <= araddr; r_len <= arlen + 1; r_beat <= 0; r_wait <= 6;
    end else if (r_busy) begin
      if (r_wait > 0) r_wait <= r_wait - 1;
      else begin
        rvalid <= 1;
        rdata  <= mem(r_base + r_beat * 4);
        rlast  <= (r_beat + 1 == r_len);
        if (r_beat + 1 == r_len) r_busy <= 0;
        r_beat <= r_beat + 1;
      end
    end
  end

  // -- ARM handshake model: the acknowledge flag and the data register ---------
  // The flag and the data are forced where the PL reads them, and change on
  // the same clock: the data is first valid when the flag is first seen.
  // arm_pulse makes the flag high for one clock, the earliest an acknowledge
  // could drop.
  reg        arm_flag = 0;
  reg [31:0] arm_data = 0;   // axi_reg1: garbage until the ARM answers
  reg [31:0] arm_next = 0;   // what the ARM answers with
  reg        arm_pulse = 0;
  integer    arm_lat = 12, arm_state = 0, arm_cnt = 0;
  always @(posedge clk) begin
    case (arm_state)
      0: if (dut.zorro_ram_read_request) begin arm_state <= 1; arm_cnt <= arm_lat; end
      1: if (arm_cnt > 0) arm_cnt <= arm_cnt - 1;
         else begin arm_flag <= 1; arm_data <= arm_next; arm_state <= 2; end
      2: if (arm_pulse) begin arm_flag <= 0; arm_state <= 3; end
         else if (!dut.zorro_ram_read_request) begin
           arm_cnt <= arm_lat; arm_state <= 4;
         end
      3: if (!dut.zorro_ram_read_request) arm_state <= 0;
      4: if (arm_cnt > 0) arm_cnt <= arm_cnt - 1;
         else begin arm_flag <= 0; arm_state <= 0; end
    endcase
  end

  // -- pin monitor ------------------------------------------------------------
  // Everything read here is the value before the edge's own updates.
  reg [31:0] pins_prev = 0;
  reg        nd_prev = 0;
  reg        assert_seen = 0;
  reg [31:0] assert_pins = 0;
  // lead: clocks from the first edge at which the pins carry the expected
  // value to the edge that sets DTACK (1 = the data is on the pins one full
  // clock before DTACK)
  reg [31:0] want_now = 0;
  reg        want_valid = 0;
  integer    ecount = 0, data_k = -1, assert_lead = 0;
  wire [31:0] pins_now = {ZORRO_DATA, ZORRO_ADDR[22:7]};
  always @(posedge clk) begin
    ecount = ecount + 1;
    if (want_valid && data_k < 0 && pins_now === want_now) data_k = ecount;
    if (ZORRO_NDTACK === 1'b1 && !nd_prev) begin
      // DTACK was set at the previous edge; pins_prev is what the pins
      // carried at that edge
      assert_seen <= 1;
      assert_pins <= pins_prev;
      assert_lead = (data_k < 0) ? -99 : ecount - data_k;
    end
    nd_prev   <= (ZORRO_NDTACK === 1'b1);
    pins_prev <= pins_now;
  end

  integer errors = 0, checks = 0, lead_min = 99, lead_max = -99;
  integer n_axi = 0, n_rxpf = 0, n_reg = 0, n_arm = 0;

  // one Zorro III longword read at absolute address a, expecting pins == want
  task z3_read(input [31:0] a, input [31:0] want, input [8*8-1:0] what);
    integer timeout;
    reg [31:0] late;
    begin
      assert_seen = 0; data_k = -1; want_now = want; want_valid = 1;
      za_drv = {a[23:2], 1'b0};
      zd_drv = {a[31:24], 8'h00};
      za_oe = 1; zd_oe = 1; ZORRO_READ = 1;
      #30 ZORRO_NFCS = 0;
      #40 begin za_oe = 0; zd_oe = 0; end
      ZORRO_DOE = 1;
      #10 begin ZORRO_NUDS = 0; ZORRO_NLDS = 0; ZORRO_NDS1 = 0; ZORRO_NDS0 = 0; end
      for (timeout = 0; timeout < 30000 && ZORRO_NDTACK !== 1'b1; timeout = timeout + 1) #1;
      if (ZORRO_NDTACK !== 1'b1) begin
        $display("FAIL %0s read %h: no DTACK", what, a); errors = errors + 1;
      end else begin
        #20;
        late = pins_now;
        checks = checks + 1;
        if (assert_lead < lead_min) lead_min = assert_lead;
        if (assert_lead > lead_max) lead_max = assert_lead;
        if (!assert_seen) begin
          $display("FAIL %0s read %h: monitor missed the DTACK edge", what, a); errors = errors + 1;
        end else if (assert_pins !== want) begin
          $display("FAIL %0s read %h: pins %h at the DTACK edge, want %h", what, a, assert_pins, want);
          errors = errors + 1;
        end
        if (late !== want) begin
          $display("FAIL %0s read %h: pins %h 20 ns after DTACK, want %h", what, a, late, want);
          errors = errors + 1;
        end
      end
      want_valid = 0;
      ZORRO_NUDS = 1; ZORRO_NLDS = 1; ZORRO_NDS1 = 1; ZORRO_NDS0 = 1;
      ZORRO_DOE = 0;
      #10 ZORRO_NFCS = 1;
      #120;
    end
  endtask

  task arm_read(input [31:0] off, input [31:0] data);
    begin
      arm_next = data;
      arm_data = ~data;
      z3_read(BOARD + off, data, "arm");
      n_arm = n_arm + 1;
    end
  endtask

  integer i;
  initial begin
    force dut.zorro_ram_read_flag = arm_flag;
    force dut.axi_reg1 = arm_data;
    dut.slv_reg0 = 0;
    dut.slv_reg4 = 3;
    repeat (20) @(posedge clk);
    dut.z3_ram_low = BOARD;
    dut.z3_confdone = 1;
    dut.zorro_state = 9;       // CONFIGURED: derives the window bounds
    repeat (4) @(posedge clk);
    dut.zorro_state = 12;      // Z3_IDLE
    repeat (10) @(posedge clk);

    // register reads, two different values back to back
    for (i = 0; i < 4; i = i + 1) begin
      z3_read(BOARD + 32'h1100, {16'h5a39, 16'h0200}, "reg");  // SDK_REG_MAGIC
      z3_read(BOARD + 32'h1108, 32'h00000000, "reg");               // SDK_REG_DOORBELL, nothing pending
      n_reg = n_reg + 2;
    end

    // card memory, single beats, different addresses
    for (i = 0; i < 6; i = i + 1) begin
      z3_read(BOARD + 32'h20000 + i * 32'h104, swapped(mem(`ARM_MEMORY_START + 32'h20000 + i * 32'h104)), "axi");
      n_axi = n_axi + 1;
    end
    z3_read(BOARD + 32'h9000, swapped(mem(`TX_FRAME_ADDRESS - 32'h8000 + 32'h9000)), "axi");
    n_axi = n_axi + 1;

    // receive window, slot 3: three read-ahead lines front to back, misses and hits
    for (i = 0; i < 40; i = i + 1) begin
      z3_read(BOARD + 32'h2000 + i * 4,
              swapped(mem(`RX_BACKLOG_ADDRESS + i * 4 + (32'd3 << 11))), "rxpf");
      n_rxpf = n_rxpf + 1;
    end

    // ARM-answered reads: acknowledge after a delay, flag held to the end
    arm_pulse = 0;
    for (i = 0; i < 4; i = i + 1) arm_read(32'h0040 + i * 4, 32'ha5c30000 ^ (i * 32'h01010101) ^ 32'h00ff1234);
    // the shortest acknowledge: the flag high for a single clock
    arm_pulse = 1; arm_lat = 3;
    for (i = 0; i < 4; i = i + 1) arm_read(32'h0080 + i * 4, 32'h3c5a0000 ^ (i * 32'h02020202) ^ 32'h0f0f4321);
    arm_pulse = 1; arm_lat = 20;
    for (i = 0; i < 2; i = i + 1) arm_read(32'h00c0 + i * 4, 32'h96000000 ^ (i * 32'h03030303) ^ 32'h00aa55c3);

    repeat (20) @(posedge clk);
    if (checks != n_axi + n_rxpf + n_reg + n_arm) begin
      $display("FAIL counted %0d cycles, ran %0d", checks, n_axi + n_rxpf + n_reg + n_arm);
      errors = errors + 1;
    end
    if (errors == 0)
      $display("RESULT PASS z3 read DTACK after data (%0d cycles: axi %0d, rxpf %0d, reg %0d, arm %0d; data on the pins %0d..%0d clock(s) before DTACK)",
               checks, n_axi, n_rxpf, n_reg, n_arm, lead_min, lead_max);
    else
      $display("RESULT FAIL z3 read DTACK before data: %0d error(s) in %0d cycles", errors, checks);
    if (errors != 0) $fatal(1, "DTACK/data order test failed");
    $finish;
  end

  initial begin #50000000 $display("RESULT FAIL timeout"); $fatal(1, "timeout"); end
endmodule
