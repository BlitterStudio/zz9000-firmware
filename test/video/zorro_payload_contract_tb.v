`timescale 1ns/1ps
/* Full-production MNTZorro public Z2/Z3 payload-register contract proof. */
module zorro_payload_contract_tb;
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
  reg [31:0] wdata = 0;
  reg awvalid = 0, wvalid = 0, arvalid = 0;
  wire awready, wready, bvalid, arready, rvalid;
  wire [31:0] rdata;
  reg [31:0] read_value, expected_addr;
  reg [3:0] expected_bytes;
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
    .source_sync_diagnostic(0), .S_AXI_ACLK(clk), .S_AXI_ARESETN(resetn),
    .S_AXI_AWADDR(awaddr), .S_AXI_AWPROT(0), .S_AXI_AWVALID(awvalid),
    .S_AXI_AWREADY(awready), .S_AXI_WDATA(wdata), .S_AXI_WSTRB(4'hf),
    .S_AXI_WVALID(wvalid), .S_AXI_WREADY(wready), .S_AXI_BVALID(bvalid),
    .S_AXI_BREADY(1), .S_AXI_ARADDR(araddr), .S_AXI_ARPROT(0),
    .S_AXI_ARVALID(arvalid), .S_AXI_ARREADY(arready), .S_AXI_RDATA(rdata),
    .S_AXI_RVALID(rvalid), .S_AXI_RREADY(1));

  task ck; input ok; input [511:0] why; begin
    checks = checks + 1;
    if (ok !== 1) begin failures = failures + 1; $display("MISMATCH %0s", why); end
  end endtask
  task clocks; input integer n; begin repeat(n) @(posedge clk); end endtask
  task axiw; input [4:0] a; input [31:0] d; begin
    @(negedge clk); awaddr=a; wdata=d; awvalid=1; wvalid=1;
    wait(awready && wready); #0.001; wait(bvalid);
    @(negedge clk); awvalid=0; wvalid=0;
  end endtask
  task axir; input [4:0] a; begin
    @(negedge clk); araddr=a; arvalid=1;
    wait(arready); #0.001; wait(rvalid); #0.001; read_value=rdata;
    @(negedge clk); arvalid=0;
  end endtask

  task acknowledge; input [31:0] expected; begin
    wait(dut.zorro_ram_write_request === 1); clocks(3);
    ck(dut.zorro_ram_write_request === 1, "request held until ARM acknowledgement");
    ck(dut.zorro_ram_write_data === expected, "captured payload and lanes remain stable");
`ifdef ZORRO2
    ck(dut.zorro_ram_write_bytes === 4'b0011, "Z2 word lanes");
`else
    ck(dut.zorro_ram_write_bytes === expected_bytes, "Z3 requested lanes");
    axir(5'h00); ck(read_value === expected_addr, "REG0 aligned Z3 address");
    axir(5'h0c); ck(read_value[31] && read_value[29:26] === expected_bytes,
                      "REG3 request and DS lanes");
`endif
    axir(5'h04); ck(read_value === expected, "REG1 ARM read is pending Zorro payload");
    axir(5'h08); ck((read_value & 32'hffe00000) === 32'h4ca00000,
                      "REG2 capture marker during request");
    axiw(0, 32'h80000000); clocks(4); axiw(0, 0);
    wait(dut.zorro_ram_write_request === 0);
  end endtask

`ifdef ZORRO2
  task setup; begin
    addr_oe=1; data_oe=1; addr_drive=23'h740024; data_drive=16'h2000;
    nccs=0; nuds=0; nlds=0; clocks(20);
    nccs=1; nuds=1; nlds=1; ncfgin=1; clocks(16);
  end endtask
  task guest; input [15:0] off; input [15:0] data; begin
    addr_drive=(24'h200000+off)>>1; data_drive=data; nccs=0; nuds=0; nlds=0;
    clocks(18); acknowledge({16'b0,data}); nccs=1; nuds=1; nlds=1; clocks(12);
  end endtask
`else
  /* Z3 multiplexes the address high byte over DATA.  Keep the address phase
   * stable before /FCS falls; only then drive data and selected halfword lanes. */
  task z3start; input [31:0] a; begin
    @(negedge clk); addr_oe=1; data_oe=1; addr_drive={a[23:2],1'b0};
    data_drive={a[31:24],8'h0}; nfcs=1; clocks(3);
    @(negedge clk); nfcs=0; clocks(9);
  end endtask
  task setup; begin
    z3start(32'hff000044); data_drive=16'h4000; nds0=0; nds1=0; nlds=0; nuds=0;
    clocks(14); nds0=1; nds1=1; nlds=1; nuds=1; clocks(5);
    nfcs=1; clocks(10); ncfgin=1; clocks(20);
  end endtask
  task guest; input [15:0] raw; input [15:0] word; input [3:0] lanes;
              input [31:0] expected; begin
    expected_addr=raw; expected_bytes=lanes; z3start(32'h40000000+raw);
    if (lanes == 4'b0011) begin
      data_drive=16'h4000; addr_drive={word,7'b0}; nds0=0; nds1=0;
    end else begin
      /* Upper halfword: address pins remain the unselected low halfword. */
      data_drive=word; addr_drive=raw>>1; nlds=0; nuds=0;
    end
    clocks(14); nds0=1; nds1=1; nlds=1; nuds=1; clocks(5);
    nfcs=1; clocks(10); acknowledge(expected); clocks(20);
  end endtask
`endif

  initial begin
    clocks(5); resetn=1; clocks(18); setup();
`ifdef ZORRO2
    guest(16'h0002,16'h0003); guest(16'h000a,16'h1357); guest(16'h000c,16'h2468);
    guest(16'h00ca,16'h0004); guest(16'h00cc,16'h0000);
`else
    guest(16'h0000,16'h0003,4'b0011,32'h40000003);
    guest(16'h0008,16'h1357,4'b0011,32'h40001357);
    guest(16'h000c,16'h2468,4'b1100,32'h24680000);
    guest(16'h00c8,16'h0004,4'b0011,32'h40000004);
    guest(16'h00cc,16'h0000,4'b1100,32'h00000000);
`endif
    if (failures == 0) $display("RESULT PASS zorro payload contract: %0d", checks);
    else $display("RESULT FAIL zorro payload contract: %0d", failures);
    $finish;
  end
  initial begin #500000; $display("RESULT FAIL zorro payload contract: timeout"); $finish; end
endmodule
