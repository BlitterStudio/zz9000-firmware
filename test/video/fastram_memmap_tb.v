`timescale 1ns/1ps
/*
 * have supplied their naturally aligned host bases. The fixed DDR landing
 * zone is 2 MiB-aligned so it sits directly below the top belt; the delta
 * implementation deliberately accepts any host 256 MiB alignment. The test
 * uses the CONFIGURED state to derive the window high bound/delta exactly as
 * the FSM does; bus timing is intentionally covered by the public payload
 * contract test.
 */
module fastram_memmap_tb;
  localparam [31:0] FAST_DDR_BASE = 32'h2fe00000;
  localparam [31:0] FAST_SIZE = 32'h10000000;
  localparam [31:0] ARM_MEMORY_START = 32'h001f0000;
  localparam CONFIGURED = 9, CONFIGURED_CLEAR = 10, Z3_CONFIGURING = 24;

  reg clk = 0, resetn = 0, e7m = 0;
  always #5 clk = ~clk;
  always #3 e7m = ~e7m;
  tri [22:0] ZORRO_ADDR;
  tri [15:0] ZORRO_DATA;
  reg [22:0] addr_drive = 0;
  reg [15:0] data_drive = 0;
  reg addr_oe = 0, data_oe = 0;
  assign ZORRO_ADDR = addr_oe ? addr_drive : 23'bz;
  assign ZORRO_DATA = data_oe ? data_drive : 16'bz;
  reg zread = 0, nuds = 1, nlds = 1, nds1 = 1, nds0 = 1;
  reg nccs = 1, nfcs = 1, ndoe = 1, niorst = 1, ncfgin = 0;
  reg [4:0] awaddr = 0, araddr = 0;
  reg [31:0] wdata = 0, read_value = 0;
  reg awvalid = 0, wvalid = 0, arvalid = 0;
  wire awready, wready, bvalid, arready, rvalid;
  wire [31:0] rdata;
  integer checks = 0, failures = 0;

  MNTZorro_v0_1_S00_AXI dut (
    .arm_interrupt(), .ZORRO_ADDR(ZORRO_ADDR), .ZORRO_DATA(ZORRO_DATA),
    .ZORRO_NBGN(1'b1), .ZORRO_READ(zread), .ZORRO_NUDS(nuds),
    .ZORRO_NLDS(nlds), .ZORRO_NDS1(nds1), .ZORRO_NDS0(nds0),
    .ZORRO_NCCS(nccs), .ZORRO_NFCS(nfcs), .ZORRO_DOE(ndoe),
    .ZORRO_NIORST(niorst), .ZORRO_NCFGIN(ncfgin), .ZORRO_E7M(e7m),
    .ZORRO_C28D(e7m), .VCAP_VSYNC(0), .VCAP_HSYNC(0),
    .VCAP_G0(0), .VCAP_G1(0), .VCAP_G2(0), .VCAP_G3(0), .VCAP_G4(0),
    .VCAP_G5(0), .VCAP_G6(0), .VCAP_G7(0), .VCAP_B0(0), .VCAP_B1(0),
    .VCAP_B2(0), .VCAP_B3(0), .VCAP_B4(0), .VCAP_B5(0), .VCAP_B6(0),
    .VCAP_B7(0), .VCAP_R0(0), .VCAP_R1(0), .VCAP_R2(0), .VCAP_R3(0),
    .VCAP_R4(0), .VCAP_R5(0), .VCAP_R6(0), .VCAP_R7(0),
    .m00_axi_aclk(clk), .m00_axi_aresetn(resetn), .m00_axi_awready(1),
    .m00_axi_wready(1), .m00_axi_bresp(0), .m00_axi_bvalid(0),
    .m00_axi_arready(1), .m00_axi_rdata(0), .m00_axi_rresp(0),
    .m00_axi_rlast(0), .m00_axi_rvalid(0), .m01_axi_aclk(clk),
    .m01_axi_aresetn(resetn), .m01_axi_awready(1), .m01_axi_wready(1),
    .m01_axi_bresp(0), .m01_axi_bvalid(0), .video_control_vblank_in(0),
    .source_sync_diagnostic(0), .S_AXI_ACLK(clk), .S_AXI_ARESETN(resetn),
    .S_AXI_AWADDR(awaddr), .S_AXI_AWPROT(0), .S_AXI_AWVALID(awvalid),
    .S_AXI_AWREADY(awready), .S_AXI_WDATA(wdata), .S_AXI_WSTRB(4'hf),
    .S_AXI_WVALID(wvalid), .S_AXI_WREADY(wready), .S_AXI_BVALID(bvalid),
    .S_AXI_BREADY(1), .S_AXI_ARADDR(araddr), .S_AXI_ARPROT(0),
    .S_AXI_ARVALID(arvalid), .S_AXI_ARREADY(arready), .S_AXI_RDATA(rdata),
    .S_AXI_RVALID(rvalid), .S_AXI_RREADY(1));
  task clocks; input integer n; begin repeat(n) @(posedge clk); #0.001; end endtask
  task ck; input ok; input [511:0] why; begin
    checks = checks + 1;
    if (ok !== 1) begin failures = failures + 1; $display("FAIL %0s", why); end
  end endtask
  task axir; input [4:0] a; begin
    @(negedge clk); araddr = a; arvalid = 1;
    wait(arready); #0.001; wait(rvalid); #0.001; read_value = rdata;
    @(negedge clk); arvalid = 0;
  end endtask
  task axiw; input [4:0] a; input [31:0] d; begin
    @(negedge clk); awaddr = a; wdata = d; awvalid = 1; wvalid = 1;
    wait(awready && wready); #0.001; wait(bvalid);
    @(negedge clk); awvalid = 0; wvalid = 0;
  end endtask
  task derive_fast_window; input [31:0] base; begin
    /* Feed the fast PIC's 0xff000044 autoconfig base write through the
     * production decoder before CONFIGURED derives its translation delta. */
    @(negedge clk);
    dut.z3_fast_low = 0;
    dut.z3_curpic = 1;
    dut.z3addr = 32'hff000044;
    dut.z3_din_high_s2 = base[31:16];
    dut.z3_ds0 = 1;
    dut.z3_ds1 = 1;
    dut.z3_ds2 = 1;
    dut.z3_ds3 = 1;
    dut.zorro_state = 46; /* Z3_AUTOCONF_WRITE */
    clocks(1);
    ck(dut.z3_fast_low === base, "fast PIC autoconfig write captures aligned host base");
    @(negedge clk);
    dut.z3_fast_high = 0;
    dut.z3_fast_ddr_delta = 0;
    dut.z3_ram_low = 32'h40000000;
    dut.fastram_ready = 0;
    dut.z3_curpic = 0;
    dut.zorro_state = CONFIGURED;
    clocks(1);
    ck(dut.z3_fast_high === base + FAST_SIZE, "aligned fast PIC gets a 256 MiB host window");
    ck(dut.z3_fast_ddr_delta === base - (FAST_DDR_BASE - ARM_MEMORY_START),
       "autoconfig base derives fixed-DDR translation delta");
  end endtask
  task check_translation; input [31:0] base; begin
    @(negedge clk); dut.z3addr = base; clocks(1);
    ck(dut.z3addr_in_ram === 1'b1, "first fast host byte is accepted");
    ck(dut.z3_mapped_addr + ARM_MEMORY_START === FAST_DDR_BASE,
       "first fast host byte lands at 0x2fe00000");
    @(negedge clk); dut.z3addr = base + FAST_SIZE - 1; clocks(1);
    ck(dut.z3addr_in_ram === 1'b1, "last fast host byte is accepted");
    ck(dut.z3_mapped_addr + ARM_MEMORY_START === FAST_DDR_BASE + FAST_SIZE - 1,
       "last fast host byte lands at 0x3fdfffff");
    @(negedge clk); dut.z3addr = base - 1; clocks(1);
    ck(dut.z3addr_in_ram === 1'b0, "byte below fast window is rejected");
    @(negedge clk); dut.z3addr = base + FAST_SIZE; clocks(1);
    ck(dut.z3addr_in_ram === 1'b0, "byte above fast window is rejected");
  end endtask

  initial begin
    clocks(5); resetn = 1; clocks(20);
    axir(5'h1c);
    ck(read_value === 32'h80000000,
       "REG7 advertises the relocated Fast-RAM DDR base to firmware");
    ck(dut.VCAP_DIAG_VARIANT_VALUE[8] === 1'b1,
       "host diagnostic variant bit records the relocated Fast-RAM base");

    ck(dut.videocap_address_valid === 1'b0,
       "capture writeback is disarmed until firmware programs an allocation");
    axiw(5'h0c, 32'h03500040); clocks(2);
    axiw(5'h08, 32'h80000021); clocks(3);
    axiw(5'h08, 0); clocks(2);
    ck(dut.videocap_address === 32'h03500040 &&
       dut.videocap_address_valid === 1'b1,
       "aligned OP_VIDEOCAP_ADDRESS arms the dynamic capture backing");
    force dut.vcap_capture_ready_axi = 1'b1;
    force dut.videocap_mode_sync = 1'b1;
    @(negedge clk);
    dut.videocap_save_state = 4'h2;
    dut.vcap_loss_pending = 0;
    dut.videocap_save_x = 0;
    dut.videocap_save_line_done = 10'h3ff;
    dut.vc_saving_line = 0;
    dut.vc_saveaddr_line = 0;
    dut.videocap_pitch_sync = 0;
    dut.vc_saveaddr1 = 0;
    dut.vc_row_base = 0;
    clocks(1);
    ck(dut.m01_axi_awaddr_out === 32'h03500040,
       "dynamic capture backing supplies the m01 AXI burst base");
    release dut.vcap_capture_ready_axi;
    release dut.videocap_mode_sync;
    axiw(5'h0c, 32'h03500041); clocks(2);
    axiw(5'h08, 32'h80000021); clocks(3);
    axiw(5'h08, 0); clocks(2);
    ck(dut.videocap_address === 32'h03500040,
       "misaligned OP_VIDEOCAP_ADDRESS is rejected without rebasing capture");

    derive_fast_window(32'h50000000); check_translation(32'h50000000);
    derive_fast_window(32'h60000000); check_translation(32'h60000000);

    /* A closed firmware gate must complete only the RTG PIC configuration. */
    @(negedge clk); dut.fastram_ready = 0; dut.z3_curpic = 0;
    dut.zorro_state = CONFIGURED; clocks(1);
    ck(dut.z3_curpic === 1'b0 && dut.zorro_state === CONFIGURED_CLEAR,
       "closed gate withholds the second Fast-RAM PIC");

    /* An open gate enters the second PIC's autoconfig state. */
    @(negedge clk); dut.fastram_ready = 1; dut.z3_curpic = 0;
    dut.zorro_state = CONFIGURED; clocks(1);
    ck(dut.z3_curpic === 1'b1 && dut.zorro_state === Z3_CONFIGURING,
       "open gate presents the second Fast-RAM PIC");

    /* A Zorro reset still revokes programmed windows while retaining the
     * firmware-owned gate latch for the normal warm-reset reconfiguration. */
    @(negedge clk); dut.slv_reg6 = 1; niorst = 0; clocks(10);
    ck(dut.z3_curpic === 1'b0 && dut.z3_fast_low === 0 && dut.z3_fast_high === 0,
       "reset revokes the configured Fast-RAM PIC and host window");
    ck(dut.fastram_ready === 1'b1, "reset preserves the firmware Fast-RAM gate latch");
    ck(dut.videocap_address === 32'h01000000 &&
       dut.videocap_address_valid === 1'b0,
       "reset restores and disarms the legacy capture address");
    @(negedge clk); niorst = 1; clocks(8);

    if (failures == 0) $display("RESULT PASS fastram memory-map contract: %0d", checks);
    else $display("RESULT FAIL fastram memory-map contract: %0d", failures);
    $finish;
  end
  initial begin #100000; $display("RESULT FAIL fastram memory-map contract: timeout"); $finish; end
endmodule
