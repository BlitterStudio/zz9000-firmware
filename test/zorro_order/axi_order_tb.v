`timescale 1ns/1ps
/*
 * Read-after-write ordering on m00, driven through production MNTZorro Z3
 * pins. The AXI model does not make a write visible until its B handshake.
 * +BYPASS_ORDERING forces the production counter low to reproduce the former
 * read overtaking a delayed write; the normal arm must observe the new word.
 */
module axi_order_tb;
  localparam [31:0] BOARD = 32'h48000000;
  localparam [31:0] OFFSET = 32'h00020000;
  localparam [31:0] NEW_VALUE = 32'h12345678;

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

  wire [31:0] awaddr, wdata, araddr;
  wire awvalid, wvalid, bready, arvalid;
  reg awready = 1, wready = 1, arready = 1;
  reg bvalid = 0;
  reg [31:0] rdata = 0;
  reg rvalid = 0, rlast = 0;

  reg [31:0] memory = 0;
  reg [31:0] pending_data = 0, read_data = 0;
  reg write_pending = 0;
  integer b_delay = -1, r_delay = -1;
  integer ar_while_write_pending = 0, stale_reads = 0;
  integer aw_count = 0, b_count = 0, errors = 0;
  reg bypass = 0;

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
    .m00_axi_awready(awready), .m00_axi_awaddr(awaddr), .m00_axi_awvalid(awvalid),
    .m00_axi_wready(wready), .m00_axi_wdata(wdata), .m00_axi_wvalid(wvalid),
    .m00_axi_bresp(2'b00), .m00_axi_bvalid(bvalid), .m00_axi_bready(bready),
    .m00_axi_arready(arready), .m00_axi_araddr(araddr), .m00_axi_arvalid(arvalid),
    .m00_axi_rdata(rdata), .m00_axi_rresp(2'b00), .m00_axi_rlast(rlast), .m00_axi_rvalid(rvalid),
    .m01_axi_aclk(clk), .m01_axi_aresetn(1'b1), .m01_axi_awready(1'b1),
    .m01_axi_wready(1'b1), .m01_axi_bresp(2'b00), .m01_axi_bvalid(1'b0),
    .video_control_vblank_in(2'b00), .source_sync_diagnostic(64'd0),
    .S_AXI_ACLK(clk), .S_AXI_ARESETN(1'b1), .S_AXI_AWADDR(0), .S_AXI_AWPROT(3'b0),
    .S_AXI_AWVALID(1'b0), .S_AXI_WDATA(0), .S_AXI_WSTRB(0), .S_AXI_WVALID(1'b0),
    .S_AXI_BREADY(1'b1), .S_AXI_ARADDR(0), .S_AXI_ARPROT(3'b0),
    .S_AXI_ARVALID(1'b0), .S_AXI_RREADY(1'b1)
  );

  always @(posedge clk) begin
    rvalid <= 0;
    rlast <= 0;
    if (awvalid && awready) begin
      write_pending <= 1;
      aw_count <= aw_count + 1;
    end
    if (wvalid && wready) begin
      pending_data <= wdata;
      b_delay <= 400;
    end
    if (b_delay > 0)
      b_delay <= b_delay - 1;
    else if (b_delay == 0 && !bvalid)
      bvalid <= 1;
    if (bvalid && bready) begin
      memory <= pending_data;
      bvalid <= 0;
      b_delay <= -1;
      write_pending <= 0;
      b_count <= b_count + 1;
    end
    if (arvalid && arready) begin
      read_data <= memory;
      r_delay <= 3;
      if (write_pending) begin
        ar_while_write_pending <= ar_while_write_pending + 1;
        $display("ORDER VIOLATION: AR accepted before prior B at %0t", $time);
      end
    end
    if (r_delay > 0)
      r_delay <= r_delay - 1;
    else if (r_delay == 0) begin
      rdata <= read_data;
      rvalid <= 1;
      rlast <= 1;
      r_delay <= -1;
    end
  end

  function [31:0] swapped(input [31:0] value);
    swapped = {value[7:0], value[15:8], value[23:16], value[31:24]};
  endfunction

  task z3_write(input [31:0] address, input [31:0] value);
    integer timeout;
    begin
      za_drv = {address[23:2], 1'b0};
      zd_drv = {address[31:24], 8'h00};
      za_oe = 1; zd_oe = 1; ZORRO_READ = 0;
      #30 ZORRO_NFCS = 0;
      #40 begin
        za_drv = {value[23:16], value[31:24], 7'b0};
        zd_drv = {value[7:0], value[15:8]};
      end
      ZORRO_DOE = 1;
      #10 begin ZORRO_NUDS = 0; ZORRO_NLDS = 0; ZORRO_NDS1 = 0; ZORRO_NDS0 = 0; end
      for (timeout = 0; timeout < 30000 && ZORRO_NDTACK !== 1'b1; timeout = timeout + 1) #1;
      if (ZORRO_NDTACK !== 1'b1) begin $display("FAIL write timed out"); errors = errors + 1; end
      #20;
      ZORRO_NUDS = 1; ZORRO_NLDS = 1; ZORRO_NDS1 = 1; ZORRO_NDS0 = 1;
      ZORRO_DOE = 0; za_oe = 0; zd_oe = 0;
      #10 ZORRO_NFCS = 1;
      #80;
    end
  endtask

  task z3_read(input [31:0] address, output [31:0] value);
    integer timeout;
    begin
      za_drv = {address[23:2], 1'b0};
      zd_drv = {address[31:24], 8'h00};
      za_oe = 1; zd_oe = 1; ZORRO_READ = 1;
      #30 ZORRO_NFCS = 0;
      #40 begin za_oe = 0; zd_oe = 0; end
      ZORRO_DOE = 1;
      #10 begin ZORRO_NUDS = 0; ZORRO_NLDS = 0; ZORRO_NDS1 = 0; ZORRO_NDS0 = 0; end
      for (timeout = 0; timeout < 30000 && ZORRO_NDTACK !== 1'b1; timeout = timeout + 1) #1;
      if (ZORRO_NDTACK !== 1'b1) begin
        $display("FAIL read timed out"); errors = errors + 1; value = 32'hxxxxxxxx;
      end else value = {dut.data_z3_hi16, dut.data_z3_low16};
      #20;
      ZORRO_NUDS = 1; ZORRO_NLDS = 1; ZORRO_NDS1 = 1; ZORRO_NDS0 = 1;
      ZORRO_DOE = 0;
      #10 ZORRO_NFCS = 1;
      #80;
    end
  endtask

  reg [31:0] got;
  initial begin
    bypass = $test$plusargs("BYPASS_ORDERING");
    if (bypass)
      force dut.m00_write_outstanding = 0;
    repeat (20) @(posedge clk);
    dut.z3_ram_low = BOARD;
    dut.z3_ram_high = BOARD + 32'h04000000;
    dut.z3_confdone = 1;
    dut.slv_reg4 = 0;
    dut.zorro_state = 9;
    repeat (4) @(posedge clk);
    dut.zorro_state = 12;
    repeat (10) @(posedge clk);

    z3_write(BOARD + OFFSET, NEW_VALUE);
    z3_read(BOARD + OFFSET, got);
    if (got !== swapped(NEW_VALUE)) begin
      stale_reads = stale_reads + 1;
      $display("STALE READ: got=%h want=%h", got, swapped(NEW_VALUE));
    end
    repeat (20) @(posedge clk);

    if (bypass) begin
      if (ar_while_write_pending == 1 && stale_reads == 1 && aw_count == 1 && b_count == 0)
        $display("RESULT RED reproduced delayed-B stale read");
      else begin
        $display("RESULT FAIL red arm ar=%0d stale=%0d aw=%0d b=%0d", ar_while_write_pending, stale_reads, aw_count, b_count);
        errors = errors + 1;
      end
    end else if (ar_while_write_pending == 0 && stale_reads == 0 && aw_count == 1 && b_count == 1)
      $display("RESULT PASS m00 write/read ordering");
    else begin
      $display("RESULT FAIL green arm ar=%0d stale=%0d aw=%0d b=%0d", ar_while_write_pending, stale_reads, aw_count, b_count);
      errors = errors + 1;
    end
    if (errors != 0) $fatal(1, "AXI ordering test failed");
    $finish;
  end
endmodule
