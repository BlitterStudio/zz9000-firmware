`timescale 1ns/1ps
/* AXI-Lite presence slot and legacy MNTZorro register-map contract proof. */
module mntzorro_presence_tb;
  reg clk = 0, resetn = 0, e7m = 0;
  always #5 clk = ~clk;
  always #3 e7m = ~e7m;

  tri [22:0] ZORRO_ADDR;
  tri [15:0] ZORRO_DATA;
  reg zread = 0, nuds = 1, nlds = 1, nds1 = 1, nds0 = 1;
  reg nccs = 1, nfcs = 1, ndoe = 1, niorst = 1, ncfgin = 0;
  reg [5:0] awaddr = 0, araddr = 0;
  reg [31:0] wdata = 0, read_value;
  reg awvalid = 0, wvalid = 0, arvalid = 0;
  wire awready, wready, bvalid, arready, rvalid;
  wire [31:0] rdata;
  localparam [63:0] SOURCE_SYNC_DIAGNOSTIC = 64'hcafebabe11223344;
  reg [223:0] saved_registers;
  integer checks = 0, failures = 0;

  MNTZorro_v0_1_S00_AXI dut (
    .arm_interrupt(), .ZORRO_ADDR(ZORRO_ADDR), .ZORRO_DATA(ZORRO_DATA),
    .ZORRO_NBGN(1'b1), .ZORRO_READ(zread), .ZORRO_NUDS(nuds),
    .ZORRO_NLDS(nlds), .ZORRO_NDS1(nds1), .ZORRO_NDS0(nds0),
    .ZORRO_NCCS(nccs), .ZORRO_NFCS(nfcs), .ZORRO_DOE(ndoe),
    .ZORRO_NIORST(niorst), .ZORRO_NCFGIN(ncfgin), .ZORRO_E7M(e7m),
    .ZORRO_C28D(e7m),
    .VCAP_VSYNC(0), .VCAP_HSYNC(0), .VCAP_G0(0), .VCAP_G1(0),
    .VCAP_G2(0), .VCAP_G3(0), .VCAP_G4(0), .VCAP_G5(0), .VCAP_G6(0),
    .VCAP_G7(0), .VCAP_B0(0), .VCAP_B1(0), .VCAP_B2(0), .VCAP_B3(0),
    .VCAP_B4(0), .VCAP_B5(0), .VCAP_B6(0), .VCAP_B7(0), .VCAP_R0(0),
    .VCAP_R1(0), .VCAP_R2(0), .VCAP_R3(0), .VCAP_R4(0), .VCAP_R5(0),
    .VCAP_R6(0), .VCAP_R7(0),
    .m00_axi_aclk(clk), .m00_axi_aresetn(resetn), .m00_axi_awready(1),
    .m00_axi_wready(1), .m00_axi_bresp(0), .m00_axi_bvalid(0),
    .m00_axi_arready(1), .m00_axi_rdata(0), .m00_axi_rresp(0),
    .m00_axi_rlast(0), .m00_axi_rvalid(0), .m01_axi_aclk(clk),
    .m01_axi_aresetn(resetn), .m01_axi_awready(1), .m01_axi_wready(1),
    .m01_axi_bresp(0), .m01_axi_bvalid(0), .video_control_vblank_in(0),
    .source_sync_diagnostic(SOURCE_SYNC_DIAGNOSTIC), .S_AXI_ACLK(clk),
    .S_AXI_ARESETN(resetn), .S_AXI_AWADDR(awaddr), .S_AXI_AWPROT(0),
    .S_AXI_AWVALID(awvalid), .S_AXI_AWREADY(awready), .S_AXI_WDATA(wdata),
    .S_AXI_WSTRB(4'hf), .S_AXI_WVALID(wvalid), .S_AXI_WREADY(wready),
    .S_AXI_BVALID(bvalid), .S_AXI_BREADY(1), .S_AXI_ARADDR(araddr),
    .S_AXI_ARPROT(0), .S_AXI_ARVALID(arvalid), .S_AXI_ARREADY(arready),
    .S_AXI_RDATA(rdata), .S_AXI_RVALID(rvalid), .S_AXI_RREADY(1));

  task ck; input ok; input [511:0] why; begin
    checks = checks + 1;
    if (ok !== 1) begin failures = failures + 1; $display("MISMATCH %0s", why); end
  end endtask
  task clocks; input integer n; begin repeat(n) @(posedge clk); end endtask
  task axiw; input [5:0] a; input [31:0] d; begin
    @(negedge clk); awaddr=a; wdata=d; awvalid=1; wvalid=1;
    wait(awready && wready); #0.001; wait(bvalid);
    @(negedge clk); awvalid=0; wvalid=0;
  end endtask
  task axir; input [5:0] a; begin
    @(negedge clk); araddr=a; arvalid=1;
    wait(arready); #0.001; wait(rvalid); #0.001; read_value=rdata;
    @(negedge clk); arvalid=0;
  end endtask

  task check_legacy_reads; begin
    axir(6'h00); ck(read_value === dut.out_reg0, "REG0 read unchanged");
    axir(6'h04); ck(read_value === dut.out_reg1, "REG1 read unchanged");
    axir(6'h08); ck(read_value === dut.out_reg2, "REG2 read unchanged");
    axir(6'h0c); ck(read_value === dut.out_reg3, "REG3 read unchanged");
    axir(6'h10); ck(read_value === SOURCE_SYNC_DIAGNOSTIC[31:0], "REG4 read unchanged");
    axir(6'h14); ck(read_value === SOURCE_SYNC_DIAGNOSTIC[63:32], "REG5 snapshot read unchanged");
    axir(6'h18); ck(read_value === (dut.sdk_aperture_layout_ack ? 32'ha5020001 : 0),
                    "REG6 layout acknowledgement read unchanged");
    axir(6'h1c); ck(read_value === dut.SDK_APERTURE_SIZE_VALUE, "REG7 aperture read unchanged");
  end endtask

  initial begin
    clocks(5); resetn=1; clocks(24);
    check_legacy_reads();

`ifdef RASTERIX
    ck(dut.VCAP_DIAG_VARIANT_VALUE[8] === 1'b1, "variant diagnostic RasterIX bit");
`else
    ck(dut.VCAP_DIAG_VARIANT_VALUE[8] === 1'b0, "variant diagnostic default bit");
`endif

    axir(6'h38);
`ifdef RASTERIX
    ck(read_value === 32'h52495831, "RasterIX presence identity");
`else
    ck(read_value === 0, "non-RasterIX presence identity");
`endif

    axiw(6'h00, 32'h10203040); ck(dut.slv_reg0 === 32'h10203040, "REG0 write unchanged");
    axiw(6'h04, 32'h11223344); ck(dut.slv_reg1 === 32'h11223344, "REG1 write unchanged");
    axiw(6'h08, 32'h12233445); ck(dut.slv_reg2 === 32'h12233445, "REG2 write unchanged");
    axiw(6'h0c, 32'h13243546); ck(dut.slv_reg3 === 32'h13243546, "REG3 write unchanged");
    axiw(6'h10, 32'h14253647); ck(dut.slv_reg4 === 32'h14253647, "REG4 write unchanged");
    axiw(6'h14, 32'h15263748); ck(dut.slv_reg5 === 32'h15263748, "REG5 write unchanged");
    axiw(6'h18, 32'h16273849); ck(dut.slv_reg6 === 32'h16273849, "REG6 write unchanged");
    saved_registers = {dut.slv_reg6, dut.slv_reg5, dut.slv_reg4, dut.slv_reg3,
                       dut.slv_reg2, dut.slv_reg1, dut.slv_reg0};
    axiw(6'h1c, 32'hffffffff);
    ck({dut.slv_reg6, dut.slv_reg5, dut.slv_reg4, dut.slv_reg3, dut.slv_reg2,
        dut.slv_reg1, dut.slv_reg0} === saved_registers, "REG7 write remains ignored");
    axiw(6'h38, 32'hdeadbeef);
    ck({dut.slv_reg6, dut.slv_reg5, dut.slv_reg4, dut.slv_reg3, dut.slv_reg2,
        dut.slv_reg1, dut.slv_reg0} === saved_registers, "presence write is ignored");
    axir(6'h38);
`ifdef RASTERIX
    ck(read_value === 32'h52495831, "presence read remains immutable");
`else
    ck(read_value === 0, "absent presence read remains immutable");
`endif
    check_legacy_reads();

    if (failures == 0) $display("RESULT PASS MNTZorro presence contract: %0d", checks);
    else $display("RESULT FAIL MNTZorro presence contract: %0d", failures);
    $finish;
  end
  initial begin #500000; $display("RESULT FAIL MNTZorro presence contract: timeout"); $finish; end
endmodule
