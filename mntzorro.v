`timescale 1 ns / 1 ps
/*
 * MNT ZZ9000 Amiga Graphics and Coprocessor Card Firmware
 * Zorro 2/3 AXI4-Lite Interface, 24-bit Video Capture (AXI DMA)
 *
 * Copyright (C) 2019-2026, Lucie L. Hartmann <lucie@mntre.com>
 *                          MNT Research GmbH, Berlin
 *                          https://mntre.com
 * Copyright (C) 2026,      Dimitris Panokostas <midwan@gmail.com>
 *
 * Contributors: _Bnu, shanshe
 *
 * More Info: https://mntre.com/zz9000
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * GNU General Public License v3.0 or later
 *
 * https://spdx.org/licenses/GPL-3.0-or-later.html
 *
 */

// ZORRO2/3 switch
//`define ZORRO2 
`define ZORRO3

// use only together with ZORRO2:
//`define VARIANT_ZZ9500        // uses Denise adapter/A500 specific video capture
//`define VARIANT_2MB           // uses only 2MB address space
`define VARIANT_SUPERDENISE   // for A500+ and super denise

//`define VARIANT_FW20
`define VARIANT_Z3_FASTRAM
`ifndef VARIANT_DISABLE_AUTOBOOT
`define VARIANT_AUTOBOOT        // enable autoboot ROM
`endif

`define C_S_AXI_DATA_WIDTH 32
`define C_S_AXI_ADDR_WIDTH 5

// Videocap sampler variant selection. This block must stay below the AXI
// width definitions: build_variant_bitstreams.sh replaces everything from
// the variant switch through C_S_AXI_DATA_WIDTH for each board target.
// Keep this order aligned with the capture-clock phase chain: the committed
// default defines both ZORRO3 and VARIANT_SUPERDENISE, and must select the
// video-slot path.
`ifdef ZORRO3
  `define VCAP_RGB_MODE     0
  `define VCAP_CSYNC_VSYNC  0
`elsif VARIANT_SUPERDENISE
  `define VCAP_RGB_MODE     0
  `define VCAP_CSYNC_VSYNC  1
`elsif VARIANT_ZZ9500
  `define VCAP_RGB_MODE     1
  `define VCAP_CSYNC_VSYNC  1
`else
  `define VCAP_RGB_MODE     2
  `define VCAP_CSYNC_VSYNC  0
`endif
`ifdef ZORRO3
  `define VCAP_FULLRATE_INT 1
`elsif VARIANT_SUPERDENISE
  `define VCAP_FULLRATE_INT 0
`elsif VARIANT_ZZ9500
  `define VCAP_FULLRATE_INT 0
`else
  `define VCAP_FULLRATE_INT 1
`endif

`ifdef VARIANT_2MB
`define RAM_SIZE 32'h200000 // 2MB for Zorro 2
`else
`define RAM_SIZE 32'h400000 // 4MB for Zorro 2
`endif
`define REG_SIZE 32'h01000
`define AUTOCONF_LOW  24'he80000
`define AUTOCONF_HIGH 24'he80080
`define Z3_SIZE_64MB 32'h04000000
`define Z3_SIZE_128MB 32'h08000000
`define Z3_SIZE_256MB 32'h10000000 // 256MB for Zorro 3
`define ARM_MEMORY_START          32'h001f0000
// Fixed DDR base for the Z3 fast-RAM PIC (VARIANT_Z3_FASTRAM). The fast
// window used to reuse the main-window translation (z3addr - z3_ram_low),
// which made its DDR placement depend on where AmigaOS autoconfig put the
// two boards relative to each other: with the canonical A3000/A4000 layout
// (RTG at 0x40000000, fast at 0x50000000) the 256 MB window landed on DDR
// 0x101f0000..0x201f0000 — overlaying the firmware's upper linker DDR, the
// SDK task queue (0x18000000) and the core-1 stack (0x1bf00000). Mapping
// the fast window to a fixed base makes the ARM memory map independent of
// board placement. Keep in sync with Z3_FASTRAM_DDR_* in
// ZZ9000_proto.sdk/ZZ9000OS/src/memorymap.h.
`define Z3_FASTRAM_ARM_BASE       32'h20000000
`define VIDEOCAP_ADDR             32'h01000000 // ARM_MEMORY_START+0xe0_0000
`define BOOT_ROM_ADDRESS          32'h3fcf0000 // 1Kb boot rom
`define TX_FRAME_ADDRESS          32'h3fd10000 // ethernet tx buffer
`define RX_FRAME_ADDRESS          32'h3fd20000 // ethernet rx buffer (unused?)
`define RX_BACKLOG_ADDRESS        32'h3fe00000 // ethernet rx buffer (128 slots x 2 KB = 256 KB)
`define USB_BLOCK_STORAGE_ADDRESS 32'h3fe40000 // must sit past RX_BACKLOG (grew from 64 KB to 256 KB)
`define FRAME_SIZE 24'h2048 // max ethernet frame size

`define C_M00_AXI_TARGET_SLAVE_BASE_ADDR 32'h10000000
`define C_M00_AXI_ID_WIDTH   1
`define C_M00_AXI_ADDR_WIDTH 32
`define C_M00_AXI_DATA_WIDTH 32
`define C_M00_AXI_AWUSER_WIDTH 0
`define C_M00_AXI_ARUSER_WIDTH 0
`define C_M00_AXI_WUSER_WIDTH 0
`define C_M00_AXI_RUSER_WIDTH 0
`define C_M00_AXI_BUSER_WIDTH 0

module MNTZorro_v0_1_S00_AXI
  (
   output wire arm_interrupt,

   inout wire [22:0] ZORRO_ADDR,
   inout wire [15:0] ZORRO_DATA,

   output wire ZORRO_INT6,
   output wire ZORRO_DATADIR,
   output wire ZORRO_ADDRDIR,
   output wire ZORRO_ADDRDIR2,
   output wire ZORRO_NBRN,
   input  wire ZORRO_NBGN,

   input wire ZORRO_READ,
   //input wire ZORRO_NMTCR,
   input wire ZORRO_NUDS,
   input wire ZORRO_NLDS,
   input wire ZORRO_NDS1,
   input wire ZORRO_NDS0,
   input wire ZORRO_NCCS,
   input wire ZORRO_NFCS,
   input wire ZORRO_DOE,
   input wire ZORRO_NIORST,
   input wire ZORRO_NCFGIN,
   input wire ZORRO_E7M,
   input wire ZORRO_C28D,

   input wire VCAP_VSYNC,
   input wire VCAP_HSYNC,
   input wire VCAP_G0,
   input wire VCAP_G1,
   input wire VCAP_G2,
   input wire VCAP_G3,
   input wire VCAP_G4,
   input wire VCAP_G5,
   input wire VCAP_G6,
   input wire VCAP_G7,

   input wire VCAP_B7,
   input wire VCAP_B6,
   input wire VCAP_B5,
   input wire VCAP_B4,
   input wire VCAP_B3,
   input wire VCAP_B2,
   input wire VCAP_B1,
   input wire VCAP_B0,

   input wire VCAP_R7,
   input wire VCAP_R6,
   input wire VCAP_R5,
   input wire VCAP_R4,
   input wire VCAP_R3,
   input wire VCAP_R2,
   input wire VCAP_R1,
   input wire VCAP_R0,

   output wire ZORRO_NCFGOUT,
   output wire ZORRO_NSLAVE,
   output wire ZORRO_NCINH,
   output wire ZORRO_NDTACK,

   //  HP master interface to write to PS memory directly
   input wire m00_axi_aclk,
   input wire m00_axi_aresetn,
   // write address channel
   input wire m00_axi_awready,
   output wire [`C_M00_AXI_ADDR_WIDTH-1 : 0] m00_axi_awaddr,
   output reg [3:0] m00_axi_awlen,
   output reg [2:0] m00_axi_awsize,
   output reg [1:0] m00_axi_awburst,
   output reg m00_axi_awlock,
   output reg [3:0] m00_axi_awcache,
   output reg [2:0] m00_axi_awprot,
   //output reg [3:0] m00_axi_awqos,
   output wire m00_axi_awvalid,

   // write channel
   input wire m00_axi_wready,
   output wire [`C_M00_AXI_DATA_WIDTH-1 : 0] m00_axi_wdata,
   output wire [`C_M00_AXI_DATA_WIDTH/8-1 : 0] m00_axi_wstrb,
   output reg m00_axi_wlast,
   output wire m00_axi_wvalid,

   // buffered write response channel
   input wire [1 : 0] m00_axi_bresp,
   input wire m00_axi_bvalid,
   output reg m00_axi_bready,

   // read address channel
   input wire m00_axi_arready,
   output reg [`C_M00_AXI_ADDR_WIDTH-1 : 0] m00_axi_araddr,
   output reg [3 : 0] m00_axi_arlen,
   output reg [2 : 0] m00_axi_arsize,
   output reg [1 : 0] m00_axi_arburst,
   output reg m00_axi_arlock,
   output reg [3 : 0] m00_axi_arcache,
   output reg [2 : 0] m00_axi_arprot,
   //output reg [3 : 0] m00_axi_arqos,
   output reg m00_axi_arvalid,

   output reg m00_axi_rready,
   input wire [`C_M00_AXI_DATA_WIDTH-1 : 0] m00_axi_rdata,
   input wire [1 : 0] m00_axi_rresp,
   input wire m00_axi_rlast,
   input wire m00_axi_rvalid,

   // HP master interface 2 to write to PS memory directly (for videocap)
   input wire m01_axi_aclk,
   input wire m01_axi_aresetn,
   // write address channel
   input wire m01_axi_awready,
   output wire [`C_M00_AXI_ADDR_WIDTH-1 : 0] m01_axi_awaddr,
   output reg [7:0] m01_axi_awlen,
   output reg [2:0] m01_axi_awsize,
   output reg [1:0] m01_axi_awburst,
   output reg m01_axi_awlock,
   output reg [3:0] m01_axi_awcache,
   output reg [2:0] m01_axi_awprot,
   output reg [3:0] m01_axi_awqos,
   output wire m01_axi_awvalid,
   // write channel
   input wire m01_axi_wready,
   output wire [`C_M00_AXI_DATA_WIDTH-1 : 0] m01_axi_wdata,
   output wire [`C_M00_AXI_DATA_WIDTH/8-1 : 0] m01_axi_wstrb,
   output reg m01_axi_wlast,
   output wire m01_axi_wvalid,
   // buffered write response channel
   input wire [1 : 0] m01_axi_bresp,
   input wire m01_axi_bvalid,
   output reg m01_axi_bready,

   // video_formatter control interface
   output reg [31:0] video_control_data_out,
   output reg [7:0]  video_control_op_out,
   output reg video_control_interlace_out,
   output reg [7:0] scanline_intensity_out,
   output reg [1:0] scanline_width_out,
   output reg        scanline_parity_out,
   output reg [7:0] scanline_intensity2_out,
   input wire [1:0] video_control_vblank_in,
   
   // ZZ9000AX peripheral reset
   output reg zz9000ax_reset_out,

   // Xilinx AXI4-Lite implementation starts here ==============================

   // Global Clock Signal
   input wire  S_AXI_ACLK,
   // Global Reset Signal. This Signal is Active LOW
   input wire  S_AXI_ARESETN,
   // Write address (issued by master, acceped by Slave)
   input wire [`C_S_AXI_ADDR_WIDTH-1 : 0] S_AXI_AWADDR,
   // Write channel Protection type. This signal indicates the
   // privilege and security level of the transaction, and whether
   // the transaction is a data access or an instruction access.
   input wire [2 : 0] S_AXI_AWPROT,
   // Write address valid. This signal indicates that the master signaling
   // valid write address and control information.
   input wire  S_AXI_AWVALID,
   // Write address ready. This signal indicates that the slave is ready
   // to accept an address and associated control signals.
   output wire  S_AXI_AWREADY,
   // Write data (issued by master, acceped by Slave)
   input wire [`C_S_AXI_DATA_WIDTH-1 : 0] S_AXI_WDATA,
   // Write strobes. This signal indicates which byte lanes hold
   // valid data. There is one write strobe bit for each eight
   // bits of the write data bus.
   input wire [(`C_S_AXI_DATA_WIDTH/8)-1 : 0] S_AXI_WSTRB,
   // Write valid. This signal indicates that valid write
   // data and strobes are available.
   input wire  S_AXI_WVALID,
   // Write ready. This signal indicates that the slave
   // can accept the write data.
   output wire  S_AXI_WREADY,
   // Write response. This signal indicates the status
   // of the write transaction.
   output wire [1 : 0] S_AXI_BRESP,
   // Write response valid. This signal indicates that the channel
   // is signaling a valid write response.
   output wire  S_AXI_BVALID,
   // Response ready. This signal indicates that the master
   // can accept a write response.
   input wire  S_AXI_BREADY,
   // Read address (issued by master, acceped by Slave)
   input wire [`C_S_AXI_ADDR_WIDTH-1 : 0] S_AXI_ARADDR,
   // Protection type. This signal indicates the privilege
   // and security level of the transaction, and whether the
   // transaction is a data access or an instruction access.
   input wire [2 : 0] S_AXI_ARPROT,
   // Read address valid. This signal indicates that the channel
   // is signaling valid read address and control information.
   input wire  S_AXI_ARVALID,
   // Read address ready. This signal indicates that the slave is
   // ready to accept an address and associated control signals.
   output wire  S_AXI_ARREADY,
   // Read data (issued by slave)
   output wire [`C_S_AXI_DATA_WIDTH-1 : 0] S_AXI_RDATA,
   // Read response. This signal indicates the status of the
   // read transfer.
   output wire [1 : 0] S_AXI_RRESP,
   // Read valid. This signal indicates that the channel is
   // signaling the required read data.
   output wire  S_AXI_RVALID,
   // Read ready. This signal indicates that the master can
   // accept the read data and response information.
   input wire  S_AXI_RREADY
   );

  // AXI4LITE signals
  reg [`C_S_AXI_ADDR_WIDTH-1 : 0]   axi_awaddr;
  reg   axi_awready;
  reg   axi_wready;
  reg [1 : 0]   axi_bresp;
  reg   axi_bvalid;
  reg [`C_S_AXI_ADDR_WIDTH-1 : 0]   axi_araddr;
  reg   axi_arready;
  reg [`C_S_AXI_DATA_WIDTH-1 : 0]   axi_rdata;
  reg [1 : 0]   axi_rresp;
  reg   axi_rvalid;

  // Example-specific design signals
  // local localparam for addressing 32 bit / 64 bit C_S_AXI_DATA_WIDTH
  // ADDR_LSB is used for addressing 32/64 bit registers/memories
  // ADDR_LSB = 2 for 32 bits (n downto 2)
  // ADDR_LSB = 3 for 64 bits (n downto 3)
  localparam integer ADDR_LSB = (`C_S_AXI_DATA_WIDTH/32) + 1;
  localparam integer OPT_MEM_ADDR_BITS = 2;
  //----------------------------------------------
  //-- Signals for user logic register space example
  //------------------------------------------------
  //-- Number of Slave Registers 4
  reg [`C_S_AXI_DATA_WIDTH-1:0] slv_reg0;
  reg [`C_S_AXI_DATA_WIDTH-1:0] slv_reg1;
  reg [`C_S_AXI_DATA_WIDTH-1:0] slv_reg2;
  reg [`C_S_AXI_DATA_WIDTH-1:0] slv_reg3;
  reg [`C_S_AXI_DATA_WIDTH-1:0] slv_reg4;
  reg [`C_S_AXI_DATA_WIDTH-1:0] slv_reg5;
  reg [`C_S_AXI_DATA_WIDTH-1:0] slv_reg6; // issue #25: Z3 fast-RAM readiness gate (PS-written)
  wire   slv_reg_rden;
  wire   slv_reg_wren;
  reg [`C_S_AXI_DATA_WIDTH-1:0]  reg_data_out;
  integer  byte_index;
  reg  aw_en;

  reg [`C_S_AXI_DATA_WIDTH-1:0] out_reg0;
  reg [`C_S_AXI_DATA_WIDTH-1:0] out_reg1;
  reg [`C_S_AXI_DATA_WIDTH-1:0] out_reg2;
  reg [`C_S_AXI_DATA_WIDTH-1:0] out_reg3;

  // I/O Connections assignments

  assign S_AXI_AWREADY  = axi_awready;
  assign S_AXI_WREADY = axi_wready;
  assign S_AXI_BRESP  = axi_bresp;
  assign S_AXI_BVALID = axi_bvalid;
  assign S_AXI_ARREADY  = axi_arready;
  assign S_AXI_RDATA  = axi_rdata;
  assign S_AXI_RRESP  = axi_rresp;
  assign S_AXI_RVALID = axi_rvalid;
  // Implement axi_awready generation
  // axi_awready is asserted for one S_AXI_ACLK clock cycle when both
  // S_AXI_AWVALID and S_AXI_WVALID are asserted. axi_awready is
  // de-asserted when reset is low.

  always @( posedge S_AXI_ACLK )
    begin
      if ( S_AXI_ARESETN == 1'b0 )
        begin
          axi_awready <= 1'b0;
          aw_en <= 1'b1;
        end
      else
        begin
          if (~axi_awready && S_AXI_AWVALID && S_AXI_WVALID && aw_en)
            begin
              // slave is ready to accept write address when
              // there is a valid write address and write data
              // on the write address and data bus. This design
              // expects no outstanding transactions.
              axi_awready <= 1'b1;
              aw_en <= 1'b0;
            end
          else if (S_AXI_BREADY && axi_bvalid)
            begin
              aw_en <= 1'b1;
              axi_awready <= 1'b0;
            end
               else
                 begin
                   axi_awready <= 1'b0;
                 end
        end
    end

  // Implement axi_awaddr latching
  // This process is used to latch the address when both
  // S_AXI_AWVALID and S_AXI_WVALID are valid.

  always @( posedge S_AXI_ACLK )
    begin
      if ( S_AXI_ARESETN == 1'b0 )
        begin
          axi_awaddr <= 0;
        end
      else
        begin
          if (~axi_awready && S_AXI_AWVALID && S_AXI_WVALID && aw_en)
            begin
              // Write Address latching
              axi_awaddr <= S_AXI_AWADDR;
            end
        end
    end

  // Implement axi_wready generation
  // axi_wready is asserted for one S_AXI_ACLK clock cycle when both
  // S_AXI_AWVALID and S_AXI_WVALID are asserted. axi_wready is
  // de-asserted when reset is low.

  always @( posedge S_AXI_ACLK )
    begin
      if ( S_AXI_ARESETN == 1'b0 )
        begin
          axi_wready <= 1'b0;
        end
      else
        begin
          if (~axi_wready && S_AXI_WVALID && S_AXI_AWVALID && aw_en )
            begin
              // slave is ready to accept write data when
              // there is a valid write address and write data
              // on the write address and data bus. This design
              // expects no outstanding transactions.
              axi_wready <= 1'b1;
            end
          else
            begin
              axi_wready <= 1'b0;
            end
        end
    end

  // Implement memory mapped register select and write logic generation
  // The write data is accepted and written to memory mapped registers when
  // axi_awready, S_AXI_WVALID, axi_wready and S_AXI_WVALID are asserted. Write strobes are used to
  // select byte enables of slave registers while writing.
  // These registers are cleared when reset (active low) is applied.
  // Slave register write enable is asserted when valid address and data are available
  // and the slave is ready to accept the write address and write data.
  assign slv_reg_wren = axi_wready && S_AXI_WVALID && axi_awready && S_AXI_AWVALID;

  always @( posedge S_AXI_ACLK )
    begin
      if ( S_AXI_ARESETN == 1'b0 )
        begin
          slv_reg0 <= 0;
          slv_reg1 <= 0;
          slv_reg2 <= 0;
          slv_reg3 <= 0;
          slv_reg4 <= 0;
          slv_reg5 <= 0;
          slv_reg6 <= 0;
        end
      else begin
        if (slv_reg_wren)
          begin
            case ( axi_awaddr[ADDR_LSB+OPT_MEM_ADDR_BITS:ADDR_LSB] )
              3'h0:
                for ( byte_index = 0; byte_index <= (`C_S_AXI_DATA_WIDTH/8)-1; byte_index = byte_index+1 )
                  if ( S_AXI_WSTRB[byte_index] == 1 ) begin
                    // Respective byte enables are asserted as per write strobes
                    // Slave register 0
                    slv_reg0[(byte_index*8) +: 8] <= S_AXI_WDATA[(byte_index*8) +: 8];
                  end
              3'h1:
                for ( byte_index = 0; byte_index <= (`C_S_AXI_DATA_WIDTH/8)-1; byte_index = byte_index+1 )
                  if ( S_AXI_WSTRB[byte_index] == 1 ) begin
                    // Respective byte enables are asserted as per write strobes
                    // Slave register 1
                    slv_reg1[(byte_index*8) +: 8] <= S_AXI_WDATA[(byte_index*8) +: 8];
                  end
              3'h2:
                for ( byte_index = 0; byte_index <= (`C_S_AXI_DATA_WIDTH/8)-1; byte_index = byte_index+1 )
                  if ( S_AXI_WSTRB[byte_index] == 1 ) begin
                    // Respective byte enables are asserted as per write strobes
                    // Slave register 2
                    slv_reg2[(byte_index*8) +: 8] <= S_AXI_WDATA[(byte_index*8) +: 8];
                  end
              3'h3:
                for ( byte_index = 0; byte_index <= (`C_S_AXI_DATA_WIDTH/8)-1; byte_index = byte_index+1 )
                  if ( S_AXI_WSTRB[byte_index] == 1 ) begin
                    // Respective byte enables are asserted as per write strobes
                    // Slave register 3
                    slv_reg3[(byte_index*8) +: 8] <= S_AXI_WDATA[(byte_index*8) +: 8];
                  end
              3'h4:
                for ( byte_index = 0; byte_index <= (`C_S_AXI_DATA_WIDTH/8)-1; byte_index = byte_index+1 )
                  if ( S_AXI_WSTRB[byte_index] == 1 ) begin
                    // Respective byte enables are asserted as per write strobes
                    // Slave register 4
                    slv_reg4[(byte_index*8) +: 8] <= S_AXI_WDATA[(byte_index*8) +: 8];
                  end
              3'h5:
                for ( byte_index = 0; byte_index <= (`C_S_AXI_DATA_WIDTH/8)-1; byte_index = byte_index+1 )
                  if ( S_AXI_WSTRB[byte_index] == 1 ) begin
                    // Respective byte enables are asserted as per write strobes
                    // Slave register 5
                    slv_reg5[(byte_index*8) +: 8] <= S_AXI_WDATA[(byte_index*8) +: 8];
                  end
              3'h6:
                for ( byte_index = 0; byte_index <= (`C_S_AXI_DATA_WIDTH/8)-1; byte_index = byte_index+1 )
                  if ( S_AXI_WSTRB[byte_index] == 1 ) begin
                    // Slave register 6 (issue #25: Z3 fast-RAM readiness flag)
                    slv_reg6[(byte_index*8) +: 8] <= S_AXI_WDATA[(byte_index*8) +: 8];
                  end
              default : begin
                slv_reg0 <= slv_reg0;
                slv_reg1 <= slv_reg1;
                slv_reg2 <= slv_reg2;
                slv_reg3 <= slv_reg3;
                slv_reg4 <= slv_reg4;
                slv_reg5 <= slv_reg5;
                slv_reg6 <= slv_reg6;
              end
            endcase
          end
      end
    end

  // Implement write response logic generation
  // The write response and response valid signals are asserted by the slave
  // when axi_wready, S_AXI_WVALID, axi_wready and S_AXI_WVALID are asserted.
  // This marks the acceptance of address and indicates the status of
  // write transaction.

  always @( posedge S_AXI_ACLK )
    begin
      if ( S_AXI_ARESETN == 1'b0 )
        begin
          axi_bvalid  <= 0;
          axi_bresp   <= 2'b0;
        end
      else
        begin
          if (axi_awready && S_AXI_AWVALID && ~axi_bvalid && axi_wready && S_AXI_WVALID)
            begin
              // indicates a valid write response is available
              axi_bvalid <= 1'b1;
              axi_bresp  <= 2'b0; // 'OKAY' response
            end                   // work error responses in future
          else
            begin
              if (S_AXI_BREADY && axi_bvalid)
                //check if bready is asserted while bvalid is high)
                //(there is a possibility that bready is always asserted high)
                begin
                  axi_bvalid <= 1'b0;
                end
            end
        end
    end

  // Implement axi_arready generation
  // axi_arready is asserted for one S_AXI_ACLK clock cycle when
  // S_AXI_ARVALID is asserted. axi_awready is
  // de-asserted when reset (active low) is asserted.
  // The read address is also latched when S_AXI_ARVALID is
  // asserted. axi_araddr is reset to zero on reset assertion.

  always @( posedge S_AXI_ACLK )
    begin
      if ( S_AXI_ARESETN == 1'b0 )
        begin
          axi_arready <= 1'b0;
          axi_araddr  <= 32'b0;
        end
      else
        begin
          if (~axi_arready && S_AXI_ARVALID)
            begin
              // indicates that the slave has acceped the valid read address
              axi_arready <= 1'b1;
              // Read address latching
              axi_araddr  <= S_AXI_ARADDR;
            end
          else
            begin
              axi_arready <= 1'b0;
            end
        end
    end

  // Implement axi_arvalid generation
  // axi_rvalid is asserted for one S_AXI_ACLK clock cycle when both
  // S_AXI_ARVALID and axi_arready are asserted. The slave registers
  // data are available on the axi_rdata bus at this instance. The
  // assertion of axi_rvalid marks the validity of read data on the
  // bus and axi_rresp indicates the status of read transaction.axi_rvalid
  // is deasserted on reset (active low). axi_rresp and axi_rdata are
  // cleared to zero on reset (active low).
  always @( posedge S_AXI_ACLK )
    begin
      if ( S_AXI_ARESETN == 1'b0 )
        begin
          axi_rvalid <= 0;
          axi_rresp  <= 0;
        end
      else
        begin
          if (axi_arready && S_AXI_ARVALID && ~axi_rvalid)
            begin
              // Valid read data is available at the read data bus
              axi_rvalid <= 1'b1;
              axi_rresp  <= 2'b0; // 'OKAY' response
            end
          else if (axi_rvalid && S_AXI_RREADY)
            begin
              // Read data is accepted by the master
              axi_rvalid <= 1'b0;
            end
        end
    end

  // Output register or memory read data
  always @( posedge S_AXI_ACLK )
    begin
      if ( S_AXI_ARESETN == 1'b0 )
        begin
          axi_rdata  <= 0;
        end
      else
        begin
          // When there is a valid read address (S_AXI_ARVALID) with
          // acceptance of read address by the slave (axi_arready),
          // output the read dada
          if (slv_reg_rden)
            begin
              axi_rdata <= reg_data_out;     // register read data
            end
        end
    end

  // end of AXI-Lite interface ==================================================

  (* mark_debug = "true" *) reg [4:0] znAS_sync;
  (* mark_debug = "true" *) reg [2:0] znUDS_sync;
  (* mark_debug = "true" *) reg [2:0] znLDS_sync;
  (* mark_debug = "true" *) reg [2:0] zREAD_sync;

  (* mark_debug = "true" *) reg [4:0] znFCS_sync;
  (* mark_debug = "true" *) reg [2:0] znDS1_sync;
  (* mark_debug = "true" *) reg [2:0] znDS0_sync;
  reg [1:0] znRST_sync;
  (* mark_debug = "true" *) reg [1:0] zDOE_sync;
  (* mark_debug = "true" *) reg [4:0] zE7M_sync;
  reg [2:0] znCFGIN_sync;

  (* mark_debug = "true" *) reg [23:0] zaddr; // zorro 2 address
  (* mark_debug = "true" *) reg [23:0] zaddr_sync;
  (* mark_debug = "true" *) reg [23:0] zaddr_sync2;
  (* mark_debug = "true" *) reg [15:0] zdata_in_sync;
  (* mark_debug = "true" *) reg [15:0] zdata_in_sync2;
  reg z2_addr_valid;
  reg [23:0] z2_mapped_addr;
  reg z2_read;
  reg z2_write;
  reg z2_datastrobe_synced;
  reg z2addr_in_ram;
  reg z2addr_in_reg;
  reg z2addr_autoconfig;
  reg [31:0] ram_low  ;//= 32'h600000;
  reg [31:0] ram_high ;//= 32'ha00000;
  reg [31:0] reg_low  ;//= 32'h601000;
  reg [31:0] reg_high ;//= 32'h602000;
  reg z2_uds;
  reg z2_lds;

  (* mark_debug = "true" *) reg [31:0] z3_ram_low  ;//= 32'h50000000;
  (* mark_debug = "true" *) reg [31:0] z3_fast_low ;
  reg [31:0] z3_ram_high ;//= 32'h50000000 + `Z3_RAM_SIZE -4;
  reg [31:0] z3_fast_high;
  // Precomputed subtrahend so the fast-window translation stays a single
  // subtraction: z3addr - z3_fast_ddr_delta, with the AXI side adding
  // ARM_MEMORY_START back, yields Z3_FASTRAM_ARM_BASE + (z3addr - z3_fast_low).
  reg [31:0] z3_fast_ddr_delta;
  (* mark_debug = "true" *) reg [31:0] z3_reg_low  ;//= 32'h50001000;
  (* mark_debug = "true" *) reg [31:0] z3_reg_high ;//= 32'h50002000;
  reg [15:0] data_z3_hi16;
  reg [15:0] data_z3_low16;
  reg z3_curpic = 0;
  // issue #25: registered copy of slv_reg6[0]. The PS sets it once the Zynq is
  // up and the Z3 fast-RAM DDR window is ready, gating the fast-RAM autoconfig PIC.
  reg fastram_ready = 0;

  (* mark_debug = "true" *) reg [15:0] data_z3_hi16_latched;
  (* mark_debug = "true" *) reg [15:0] data_z3_low16_latched;

  (* mark_debug = "true" *) reg [15:0] z3_din_high_s2;
  (* mark_debug = "true" *) reg [15:0] z3_din_low_s2;
  (* mark_debug = "true" *) reg [31:0] z3addr;
  (* mark_debug = "true" *) reg [31:0] last_z3addr;
  (* mark_debug = "true" *) reg [31:0] z3addr2;
  (* mark_debug = "true" *) reg [31:0] z3_mapped_addr;
  (* mark_debug = "true" *) reg [31:0] z3_read_addr;
  (* mark_debug = "true" *) reg [15:0] z3_read_data;
  (* mark_debug = "true" *) reg z3_fcs_state;
  (* mark_debug = "true" *) reg z3_end_cycle;

  (* mark_debug = "true" *) reg z3addr_in_ram;
  (* mark_debug = "true" *) reg z3addr_in_reg;
  (* mark_debug = "true" *) reg z3addr_autoconfig;

`ifdef ZORRO3
  reg ZORRO3 = 1;
`else
  reg ZORRO3 = 0;
`endif
  (* mark_debug = "true" *) reg dataout;
  (* mark_debug = "true" *) reg dataout_z3;
  (* mark_debug = "true" *) reg dataout_enable;
  (* mark_debug = "true" *) reg slaven;
  (* mark_debug = "true" *) reg dtack;

  reg z_reset;
  reg z_reset_delayed;
  reg z_cfgin;
  reg z_cfgin_lo;
  reg z3_confdone;

  (* mark_debug = "true" *) reg zorro_read;
  (* mark_debug = "true" *) reg zorro_write;

  (* mark_debug = "true" *) reg zorro_interrupt_req = 0;
  reg [7:0] zorro_interrupt_len = 'hff; // FIXME
  (* mark_debug = "true" *) reg zorro_interrupt_pulse = 1;
  assign ZORRO_INT6 = zorro_interrupt_pulse;

  reg [15:0] data_in;
  reg [31:0] rr_data;
  reg [15:0] data_out;
  reg [15:0] regdata_in;

  // ram arbiter
  (* mark_debug = "true" *) reg zorro_ram_read_request;
  (* mark_debug = "true" *) reg zorro_ram_write_request;
  (* mark_debug = "true" *) reg [31:0] zorro_ram_read_addr;
  (* mark_debug = "true" *) reg [3:0] zorro_ram_read_bytes;
  (* mark_debug = "true" *) reg [31:0] zorro_ram_write_addr;
  (* mark_debug = "true" *) reg [31:0] zorro_ram_write_data;
  (* mark_debug = "true" *) reg [3:0] zorro_ram_write_bytes;

  reg [15:0] default_data = 'hffff; // causes read/write glitches on A2000 (data bus interference) when 0
  reg [1:0] zorro_write_capture_bytes;
  reg [15:0] zorro_write_capture_data;

  // z3 strobes
  reg z3_ds3;
  reg z3_ds2;
  reg z3_ds1;
  reg z3_ds0;

  // level shifter direction pins
  assign ZORRO_DATADIR     = ZORRO_DOE & (dataout_enable | dataout_z3); // d2-d9  d10-15, d0-d1
  assign ZORRO_ADDRDIR     = ZORRO_DOE & (dataout_z3); // a16-a23 <- input  a8-a15 <- input
  assign ZORRO_ADDRDIR2    = 0; //ZORRO_DOE & (dataout_z3_latched);
  assign ZORRO_NBRN = 1; // TODO busmastering

  // data/addr out signals are gated by master's DOE signal
  wire ZORRO_DATA_T = ~(ZORRO_DOE & (dataout_enable | dataout_z3));
  wire ZORRO_ADDR_T = ~(ZORRO_DOE & dataout_z3);
  wire [15:0] ZORRO_DATA_IN;
  wire [22:0] ZORRO_ADDR_IN;

  // autoconf output signal
  reg z_confout = 0;
  assign ZORRO_NCFGOUT = ZORRO_NCFGIN?1'b1:(~z_confout);

  reg z_ovr = 0;
`ifdef ZORRO3
  wire [31:0] z3_addr_phase_bus = {ZORRO_DATA_IN[15:8], ZORRO_ADDR_IN[22:1], 2'b00};
  wire z3_addr_phase_autoconfig = (z3_addr_phase_bus[31:16] == 16'hff00);
  wire z3_addr_phase_in_reg = (z3_addr_phase_bus >= z3_reg_low) && (z3_addr_phase_bus < z3_reg_high);
`ifdef VARIANT_Z3_FASTRAM
  wire z3_addr_phase_in_ram = ((z3_addr_phase_bus >= z3_ram_low) && (z3_addr_phase_bus < z3_ram_high)) ||
                              ((z3_addr_phase_bus >= z3_fast_low) && (z3_addr_phase_bus < z3_fast_high));
`else
  wire z3_addr_phase_in_ram = (z3_addr_phase_bus >= z3_ram_low) && (z3_addr_phase_bus < z3_ram_high);
`endif
  // Autoconfig claims must still be qualified by raw /CFGIN so ZZ9000 does not
  // assert /SLAVE+/CINH while another card owns the autoconfig slot.  Do not use
  // the synchronized z_cfgin here: its ACLK latency is too slow for the Z3
  // address phase, where /SLAVE+/CINH must be valid shortly after /FCS falls.
  wire z3_addr_phase_match = (!z_confout && !ZORRO_NCFGIN && z3_addr_phase_autoconfig) ||
                             (z_confout && (z3_addr_phase_in_reg || z3_addr_phase_in_ram));
  wire z3_fcs_reset = !ZORRO_NIORST;
  wire z3_nslave_out;
  wire z3_ncinh_out;
  (* mark_debug = "true" *) wire z3_addr_phase_claim = z3_ncinh_out;

  // Use /FCS as a DDR output clock: the falling edge captures the decoded
  // address-phase claim, and the rising edge releases it for the next cycle.
  ODDR #(
    .DDR_CLK_EDGE("OPPOSITE_EDGE"),
    .INIT(1'b1),
    .SRTYPE("ASYNC")
  ) z3_nslave_oddr (
    .Q(z3_nslave_out),
    .C(ZORRO_NFCS),
    .CE(1'b1),
    .D1(1'b1),
    .D2(!z3_addr_phase_match),
    .R(1'b0),
    .S(z3_fcs_reset)
  );

  ODDR #(
    .DDR_CLK_EDGE("OPPOSITE_EDGE"),
    .INIT(1'b0),
    .SRTYPE("ASYNC")
  ) z3_ncinh_oddr (
    .Q(z3_ncinh_out),
    .C(ZORRO_NFCS),
    .CE(1'b1),
    .D1(1'b0),
    .D2(z3_addr_phase_match),
    .R(z3_fcs_reset),
    .S(1'b0)
  );

  assign ZORRO_NCINH = z3_ncinh_out; // inverse
  assign ZORRO_NSLAVE = z3_nslave_out;
`else
  assign ZORRO_NCINH = z_ovr?1'b1:1'b0; // inverse
  assign ZORRO_NSLAVE = (ZORRO_DOE & slaven)?1'b0:1'b1; // cannot gate by FCS for Z2
`endif
  assign ZORRO_NDTACK = (ZORRO_DOE & dtack) ?1'b1:1'b0; // inverse, pull-down transistor on output
  wire [22:0] z3_addr_out = {data_z3_low16_latched, 7'bZZZ_ZZZZ}; // FIXME this creates tri-cell warning?
  //wire [22:0] z3_addr_out = {data_z3_low16_latched, 7'b111_1111}; // FIXME this creates tri-cell warning?

  genvar i;

  generate
    for (i=0; i < 16; i=i+1) begin : ZORRO_DATABUS
      IOBUF u_iobuf_dq
           (
            .I  (ZORRO3 ? data_z3_hi16_latched[i] : data_out[i]),
            .T  (ZORRO_DATA_T),
            .IO (ZORRO_DATA[i]),
            .O  (ZORRO_DATA_IN[i])
            );
    end
  endgenerate

  generate
    for (i=0; i < 23; i=i+1) begin : ZORRO_ADDRBUS
      IOBUF u_iobuf_dq
           (
            .I  (z3_addr_out[i]),
            .T  (ZORRO_ADDR_T),
            .IO (ZORRO_ADDR[i]),
            .O  (ZORRO_ADDR_IN[i])
            );
    end
  endgenerate

`ifdef VARIANT_FW20
    assign arm_interrupt = zorro_ram_write_request | zorro_ram_read_request;
`else
    assign arm_interrupt = video_control_vblank_in[1];
`endif
  // -- synchronizers ------------------------------------------
  always @(posedge S_AXI_ACLK) begin
    znUDS_sync  <= {znUDS_sync[1:0],ZORRO_NUDS};
    znLDS_sync  <= {znLDS_sync[1:0],ZORRO_NLDS};
    znAS_sync   <= {znAS_sync[3:0],ZORRO_NCCS};
    zREAD_sync  <= {zREAD_sync[1:0],ZORRO_READ};

    znDS1_sync  <= {znDS1_sync[1:0],ZORRO_NDS1};
    znDS0_sync  <= {znDS0_sync[1:0],ZORRO_NDS0};
    znFCS_sync  <= {znFCS_sync[3:0],ZORRO_NFCS};
    znCFGIN_sync<= {znCFGIN_sync[1:0],ZORRO_NCFGIN};
    zDOE_sync   <= {zDOE_sync[0],ZORRO_DOE};

    znRST_sync  <= {znRST_sync[0],ZORRO_NIORST};

    // Z2 ------------------------------------------------
`ifdef ZORRO2
    // READ and nAS can happen dangerously close to each other. so we delay
    // the recognition of a valid Z2 cycle 2 clocks more than the other signals.
    z2_addr_valid <= (znAS_sync[4]==0 && znAS_sync[3]==0);
    z2_read  <= (zREAD_sync[2] == 1'b1);
    z2_write <= (zREAD_sync[2] == 1'b0);

    zaddr <= ZORRO_ADDR_IN[22:0];
    zaddr_sync  <= zaddr;
    zaddr_sync2 <= zaddr_sync;

    z2_mapped_addr <= {zaddr_sync2[22:0],1'b0};

    z2_datastrobe_synced <= ((znUDS_sync[2]==0 && znUDS_sync[1]==0) || (znLDS_sync[2]==0 && znLDS_sync[1]==0));
    z2_uds <= (znUDS_sync[2]==0 && znUDS_sync[1]==0);
    z2_lds <= (znLDS_sync[2]==0 && znLDS_sync[1]==0);

    zdata_in_sync2 <= ZORRO_DATA_IN;
    zdata_in_sync <= zdata_in_sync2;

    z2addr_in_ram <= (z2_mapped_addr>=ram_low && z2_mapped_addr<ram_high);
    z2addr_in_reg <= (z2_mapped_addr>=reg_low && z2_mapped_addr<reg_high);

    // FIXME was 1
    if (znAS_sync[4]==0 && z2_mapped_addr>=`AUTOCONF_LOW && z2_mapped_addr<`AUTOCONF_HIGH)
      z2addr_autoconfig <= 1'b1;
    else
      z2addr_autoconfig <= 1'b0;
`endif

    // Z3 ------------------------------------------------
`ifdef ZORRO3
    // sample z3addr on falling edge of /FCS
    // according to Z3 spec, we have max 25ns to react to falling FCS.
    case (znFCS_sync[1:0])
      2'b01: begin
        z3_fcs_state <= 1;
        z3addr <= 0;
      end
      2'b10: begin
        z3_fcs_state <= 0;
        z3addr <= z3addr2;
      end
    endcase

    z3addr2 <= {ZORRO_DATA_IN[15:8],ZORRO_ADDR_IN[22:1],2'b00};
`ifdef VARIANT_Z3_FASTRAM
    z3addr_in_ram <= ((z3addr >= z3_ram_low) && (z3addr < z3_ram_high) || (z3addr >= z3_fast_low) && (z3addr < z3_fast_high));
`else
    z3addr_in_ram <= (z3addr >= z3_ram_low) && (z3addr < z3_ram_high);
`endif
    z3addr_in_reg <= (z3addr >= z3_reg_low) && (z3addr < z3_reg_high);

    z3_ds0 <= ~znDS0_sync[1];
    z3_ds1 <= ~znDS1_sync[1];
    z3_ds2 <= ~znLDS_sync[1];
    z3_ds3 <= ~znUDS_sync[1];

    z3addr_autoconfig <= (z3addr[31:16]=='hff00);

`ifdef VARIANT_Z3_FASTRAM
    // Fast-window accesses translate to the fixed Z3_FASTRAM_ARM_BASE DDR
    // range instead of reusing the main-window offset (see define above).
    // While the fast PIC is unconfigured, z3_fast_high is 0 and the main
    // mapping is used. The comparators mirror z3addr_in_ram above, so
    // synthesis shares them. Fast-mapped offsets start at 0x1fe10000 and
    // can never alias the <0x2000 register decode or the 0x2000..0x10000
    // eth/USB/bootrom sub-windows.
    if ((z3addr >= z3_fast_low) && (z3addr < z3_fast_high))
      z3_mapped_addr <= (z3addr-z3_fast_ddr_delta);
    else
      z3_mapped_addr <= (z3addr-z3_ram_low);
`else
    z3_mapped_addr <= (z3addr-z3_ram_low);
`endif

    z3_din_high_s2 <= ZORRO_DATA_IN;       //zD[15:0];
    z3_din_low_s2  <= ZORRO_ADDR_IN[22:7]; //zA[22:7];

    // pipelined for better timing
    data_z3_hi16_latched  <= data_z3_hi16;
    data_z3_low16_latched <= data_z3_low16;

    zorro_read  <= zREAD_sync[0];
    zorro_write <= ~zREAD_sync[0];
`endif

    z_reset_delayed <= (znRST_sync==2'b00);
    z_reset <= z_reset_delayed;
    z_cfgin <= (znCFGIN_sync==3'b000);
    z_cfgin_lo <= (znCFGIN_sync==3'b111);
  end // always @ (posedge S_AXI_ACLK)

  reg [15:0] REVISION = 'h7a09; // z9

  localparam [15:0] SDK_REG_MAGIC_VALUE = 16'h5a39;
  localparam [15:0] SDK_REG_VERSION = 16'h0200;
  localparam [31:0] SDK_MAILBOX_ARM_ADDRESS = 32'h3fe43000;
  localparam [15:0] SDK_REG_MAGIC = 16'h0100;
  localparam [15:0] SDK_REG_VERSION_OFFS = 16'h0102;
  localparam [15:0] SDK_REG_MAILBOX_HI = 16'h0104;
  localparam [15:0] SDK_REG_MAILBOX_LO = 16'h0106;
  localparam [15:0] SDK_REG_DOORBELL = 16'h0108;
  localparam [15:0] SDK_REG_STATUS = 16'h010a;
  localparam [15:0] SDK_REG_IRQ_ACK = 16'h010c;
  // Z3 word writes on the low data lane arrive with bit 1 set in REGWRITE.
  localparam [15:0] SDK_REG_DOORBELL_Z3_LO = 16'h010a;
  localparam [15:0] SDK_REG_IRQ_ACK_Z3_LO = 16'h010e;
  localparam [15:0] SDK_REG_DIAG_WRITE = 16'h0110;
  localparam [15:0] SDK_REG_DIAG_WRITE_LO = 16'h0112;
  localparam [15:0] SDK_REG_DIAG_DATA = 16'h0114;
  localparam [15:0] SDK_REG_DIAG_DATA_LO = 16'h0116;
  localparam [15:0] SDK_REG_DIAG_Z3ADDR = 16'h0118;
  localparam [15:0] SDK_REG_DIAG_Z3ADDR_LO = 16'h011a;
  localparam [15:0] SDK_REG_OFFSET_MASK = 16'h0fff;
  localparam [31:0] SDK_CTRL_DOORBELL_CLEAR = 32'h20000000;
  localparam [31:0] SDK_CTRL_IRQ_ACK_CLEAR = 32'h10000000;

  // main FSM
  localparam RESET = 0;
  localparam Z2_CONFIGURING = 1;
  localparam Z2_IDLE = 2;
  localparam WAIT_WRITE = 3;
  localparam WAIT_WRITE2 = 4;
  localparam Z2_WRITE_FINALIZE = 5;
  localparam WAIT_READ = 6;
  localparam WAIT_READ2 = 7;
  localparam WAIT_READ3 = 8;

  localparam CONFIGURED = 9;
  localparam CONFIGURED_CLEAR = 10;
  localparam DECIDE_Z2_Z3 = 11;

  localparam Z3_IDLE = 12;
  localparam Z3_WRITE_UPPER = 13;
  localparam Z3_WRITE_LOWER = 14;
  localparam Z3_READ_UPPER = 15;
  localparam Z3_READ_LOWER = 16;
  localparam Z3_READ_DELAY = 17;
  localparam Z3_READ_DELAY1 = 18;
  localparam Z3_READ_DELAY2 = 19;
  localparam Z3_WRITE_PRE = 20;
  localparam Z3_WRITE_FINALIZE = 21;
  localparam Z3_ENDCYCLE = 22;
  localparam Z3_DTACK = 23;
  localparam Z3_CONFIGURING = 24;

  localparam Z2_REGWRITE = 25;
  localparam REGWRITE = 26;
  localparam REGREAD = 27;
  localparam Z2_REGREAD_POST = 28;
  localparam Z3_REGREAD_POST = 29;
  localparam Z3_REGWRITE = 30;
  localparam Z2_REGREAD = 31;
  localparam Z3_REGREAD = 32;

  localparam Z2_PRE_CONFIGURED = 34;
  localparam Z2_ENDCYCLE = 35;

  localparam WAIT_WRITE_DMA_Z2 = 36;
  localparam WAIT_WRITE_DMA_Z2_FINALIZE = 37;

  localparam RESET_DVID = 39;
  localparam COLD = 40;

  localparam WAIT_READ2B = 41; // delay states
  localparam WAIT_READ2C = 42;

  localparam WAIT_WRITE_DMA_Z3 = 43;
  localparam WAIT_WRITE_DMA_Z3_FINALIZE = 44;

  localparam Z3_AUTOCONF_READ = 45;
  localparam Z3_AUTOCONF_WRITE = 46;
  localparam Z3_AUTOCONF_READ_DLY = 47;
  localparam Z3_AUTOCONF_READ_DLY2 = 48;

  localparam Z3_REGWRITE_PRE = 49;
  localparam Z3_REGREAD_PRE = 50;
  localparam Z3_WRITE_PRE2 = 51;
  localparam WAIT_WRITE_DMA_Z3B = 52;
  localparam WAIT_WRITE_DMA_Z3C = 53;
  localparam WAIT_READ_DMA_Z3 = 54;
  localparam WAIT_READ_DMA_Z3B = 55;
  localparam WAIT_READ_DMA_Z3C = 56;
  
  localparam WAIT_READ2D = 57;
  localparam WAIT_READ3B = 58;
  localparam WAIT_READ3C = 59;
  localparam Z3_WRITE_FINALIZE2 = 60;
  localparam Z2_WRITE_FINALIZE2 = 61;

  (* mark_debug = "true" *) reg [7:0] zorro_state = COLD;
  reg [7:0] dtack_counter;
`ifdef ZORRO2
  // experimentally found *2* for TF536
  // FIXME low values slow down z2 on a3000
  reg [5:0] dtack_timeout = 2; // number of cycles before we turn off our dtack signal
`else
  reg [5:0] dtack_timeout = 6; // number of cycles before we turn off our dtack signal
`endif

  (* mark_debug = "true" *) reg [31:0] debug_counter = 0;

  reg [23:0] last_addr;
  reg [23:0] last_read_addr;
  reg [15:0] last_data;
  reg [15:0] last_read_data;

  reg [15:0] zaddr_regpart;
  reg [15:0] z3addr_regpart;
  reg [15:0] regread_addr;
  reg [15:0] regwrite_addr;

  reg [31:0] axi_reg0;
  reg [31:0] axi_reg1;
  reg [31:0] axi_reg2;
  reg [31:0] axi_reg3;
  reg [31:0] axi_reg4;
  (* mark_debug = "true" *) reg [31:0] axi_reg5;
  reg [20:0] eth_rx_frame_select;
  reg sdk_doorbell_pending;
  reg sdk_irq_ack_pending;
  reg [15:0] sdk_last_regwrite_addr;
  reg [15:0] sdk_last_regwrite_data;
  reg [3:0] sdk_last_regwrite_strobes;
  reg [31:0] sdk_last_z3addr;
  reg [15:0] sdk_doorbell_early_count;
  reg [15:0] sdk_irq_ack_early_count;

  reg [31:0] video_control_data_zorro;
  reg [7:0] video_control_op_zorro;
  reg [31:0] video_control_data_axi;
  reg [7:0] video_control_op_axi;
  reg video_control_axi;

  reg [31:0] video_control_data; // to output
  reg [7:0]  video_control_op;   // to output
  reg        video_control_vblank; // from input
  reg        video_control_hblank; // from input
  reg        video_control_interlace;
  reg [7:0] scanline_intensity  = 8'h00;
  reg [1:0] scanline_width      = 2'b00;
  reg        scanline_parity    = 1'b0;
  reg [7:0] scanline_intensity2 = 8'h00;

  reg zorro_ram_read_flag;
  reg zorro_ram_write_flag ;

  reg videocap_mode;
  reg videocap_mode_in;
  reg [31:0] videocap_address = `VIDEOCAP_ADDR;
  reg [1:0] videocap_sample_mode = 2'd0;
  reg videocap_full_width = 1'b0;
  reg [11:0] videocap_crop_h = 12'd188;
  reg [11:0] videocap_crop_v = 12'd26;
  reg [9:0] videocap_y_sync;
  reg [9:0] videocap_ymax_sync;
  reg [11:0] videocap_save_x;

  wire [10:0] vcap_x;
  wire [10:0] vcap_y;
  wire [10:0] vcap_ymax;
  wire vcap_interlace;
  wire vcap_ntsc;
  wire vcap_x_done;
  wire vcap_shres;
  wire [31:0] vcap_rdata;
  wire [11:0] vcap_raddr = videocap_save_x;

  reg E7M_PSEN = 0;
  reg E7M_PSINCDEC = 0;
  reg E7M_RESET = 0;
  reg E7M_PWRDWN = 0;

  wire clkfbout_zz9000_ps_clk_wiz_1_0;
  wire e7m_shifted;
  wire e7m_shifted180;

  // video capture clock adjustment
  MMCME2_ADV #(
               .BANDWIDTH("OPTIMIZED"),
               .CLKFBOUT_MULT_F(32.000000),
               .CLKFBOUT_PHASE(0.000000),
               .CLKFBOUT_USE_FINE_PS("TRUE"),
               .CLKIN1_PERIOD(35.000000),
               .CLKIN2_PERIOD(0.000000),
`ifdef ZORRO3
               .CLKOUT0_DIVIDE_F(8.000000),
`elsif VARIANT_SUPERDENISE
               .CLKOUT0_DIVIDE_F(16.000000),
`elsif VARIANT_ZZ9500
               .CLKOUT0_DIVIDE_F(16.000000),
`else
               .CLKOUT0_DIVIDE_F(8.000000),
`endif
               .CLKOUT0_DUTY_CYCLE(0.500000),

`ifdef ZORRO3
               .CLKOUT0_PHASE(90.000000),
`elsif VARIANT_SUPERDENISE
               .CLKOUT0_PHASE(0.000000),
`elsif VARIANT_ZZ9500
               .CLKOUT0_PHASE(90.000000),
`else
               .CLKOUT0_PHASE(270.000000),
`endif

               .CLKOUT0_USE_FINE_PS("TRUE"),
               .CLKOUT1_DIVIDE(32),
               .CLKOUT1_DUTY_CYCLE(0.500000),

`ifdef ZORRO3
               .CLKOUT1_PHASE(0.000000),
`elsif VARIANT_SUPERDENISE
               .CLKOUT1_PHASE(0.000000),
`elsif VARIANT_ZZ9500
               .CLKOUT1_PHASE(270.000000),
`else
               .CLKOUT1_PHASE(135.000000),
`endif

               .CLKOUT1_USE_FINE_PS("TRUE"),
               .COMPENSATION("ZHOLD"),
               .DIVCLK_DIVIDE(1),
               .IS_CLKINSEL_INVERTED(1'b0),
               .IS_PSEN_INVERTED(1'b0),
               .IS_PSINCDEC_INVERTED(1'b0),
               .IS_PWRDWN_INVERTED(1'b0),
               .IS_RST_INVERTED(1'b0),
               .REF_JITTER1(0.001000),
               .REF_JITTER2(0.001000),
               .SS_EN("FALSE"),
               .SS_MODE("CENTER_HIGH"),
               .SS_MOD_PERIOD(10000),
               .STARTUP_WAIT("TRUE"))
  mmcm_adv_inst
    (.CLKFBIN(clkfbout_zz9000_ps_clk_wiz_1_0),
     .CLKFBOUT(clkfbout_zz9000_ps_clk_wiz_1_0),
     //.CLKFBOUTB(NLW_mmcm_adv_inst_CLKFBOUTB_UNCONNECTED),
     //.CLKFBSTOPPED(NLW_mmcm_adv_inst_CLKFBSTOPPED_UNCONNECTED),
     .CLKIN1(ZORRO_E7M),
     .CLKIN2(1'b0),
     .CLKINSEL(1'b1),
     //.CLKINSTOPPED(NLW_mmcm_adv_inst_CLKINSTOPPED_UNCONNECTED),
     .CLKOUT0(e7m_shifted),
     //.CLKOUT0B(NLW_mmcm_adv_inst_CLKOUT0B_UNCONNECTED),
     .CLKOUT1(e7m_shifted180),
     //.CLKOUT1B(NLW_mmcm_adv_inst_CLKOUT1B_UNCONNECTED),

     .DADDR({1'b0,1'b0,1'b0,1'b0,1'b0,1'b0,1'b0}),
     .DCLK(1'b0),
     .DEN(1'b0),
     .DI({1'b0,1'b0,1'b0,1'b0,1'b0,1'b0,1'b0,1'b0,1'b0,1'b0,1'b0,1'b0,1'b0,1'b0,1'b0,1'b0}),
     //.DO(NLW_mmcm_adv_inst_DO_UNCONNECTED[15:0]),
     //.DRDY(NLW_mmcm_adv_inst_DRDY_UNCONNECTED),
     .DWE(1'b0),
     //.LOCKED(NLW_mmcm_adv_inst_LOCKED_UNCONNECTED),
     .PSCLK(S_AXI_ACLK),
     //.PSDONE(psdone),
     .PSEN(E7M_PSEN),
     .PSINCDEC(E7M_PSINCDEC),
     .PWRDWN(E7M_PWRDWN),
     .RST(E7M_RESET));

  videocap_sampler #(
      .BUF_DEPTH(2048),
      .RGB_MODE(`VCAP_RGB_MODE),
      .CSYNC_VSYNC(`VCAP_CSYNC_VSYNC),
      .FULLRATE(`VCAP_FULLRATE_INT)
  ) videocap_sampler_inst (
      .cap_clk(e7m_shifted),
      .vcap_vsync(VCAP_VSYNC),
      .vcap_hsync(VCAP_HSYNC),
      .vcap_r({VCAP_R7, VCAP_R6, VCAP_R5, VCAP_R4,
               VCAP_R3, VCAP_R2, VCAP_R1, VCAP_R0}),
      .vcap_g({VCAP_G7, VCAP_G6, VCAP_G5, VCAP_G4,
               VCAP_G3, VCAP_G2, VCAP_G1, VCAP_G0}),
      .vcap_b({VCAP_B7, VCAP_B6, VCAP_B5, VCAP_B4,
               VCAP_B3, VCAP_B2, VCAP_B1, VCAP_B0}),
      .ctl_sample_mode(videocap_sample_mode),
      .ctl_full_width(videocap_full_width),
      .ctl_crop_h(videocap_crop_h),
      .ctl_crop_v(videocap_crop_v),
      .cap_x(vcap_x),
      .cap_y(vcap_y),
      .cap_ymax(vcap_ymax),
      .cap_interlace(vcap_interlace),
      .cap_ntsc(vcap_ntsc),
      .cap_x_done(vcap_x_done),
      .cap_shres(vcap_shres),
      .axi_clk(S_AXI_ACLK),
      .buf_raddr(vcap_raddr),
      .buf_rdata(vcap_rdata)
  );
  reg [11:0] videocap_pitch;
  reg [11:0] videocap_pitch_sync;
  reg [9:0]  videocap_save_line_done;
  reg [31:0] videocap_save_addr;
  reg [3:0]  videocap_save_state = 0;

  reg videocap_mode_sync;

  reg [31:0] m01_axi_awaddr_out;
  reg [31:0] m01_axi_wdata_out;
  reg m01_axi_awvalid_out = 0;
  reg m01_axi_wvalid_out = 0;

  reg [31:0] m00_axi_awaddr_z3;
  reg [31:0] m00_axi_wdata_z3;
  reg m00_axi_awvalid_z3 = 0;
  reg m00_axi_wvalid_z3 = 0;
  reg [3:0] m00_axi_wstrb_z3;

  assign m00_axi_awaddr  = m00_axi_awaddr_z3;
  assign m00_axi_awvalid = m00_axi_awvalid_z3;
  assign m00_axi_wdata   = m00_axi_wdata_z3;
  assign m00_axi_wstrb   = m00_axi_wstrb_z3;
  assign m00_axi_wvalid  = m00_axi_wvalid_z3;

  assign m01_axi_awaddr  = m01_axi_awaddr_out;
  assign m01_axi_awvalid = m01_axi_awvalid_out;
  assign m01_axi_wdata   = m01_axi_wdata_out;
  assign m01_axi_wstrb   = 4'b1111;
  assign m01_axi_wvalid  = m01_axi_wvalid_out;

  // AXI DMA defaults
  always @(posedge S_AXI_ACLK) begin
    m00_axi_awlen <= 'h0; // 1 burst (1 write)
    m00_axi_awsize <= 'h2; // 2^2 == 4 bytes
    m00_axi_awburst <= 'h0; // FIXED (non incrementing)
    m00_axi_awcache <= 'h3;
    m00_axi_awlock <= 'h0;
    m00_axi_awprot <= 'h0;
    //m00_axi_awqos <= 'h0;
    m00_axi_wlast <= 'h1;
    m00_axi_bready <= 'h1;

    m00_axi_arlen <= 'h0;
    m00_axi_arsize <= 'h2;
    m00_axi_arburst <= 'h0;
    m00_axi_arcache <= 'hf; //was 3
    m00_axi_arlock <= 'h0;
    m00_axi_arprot <= 'h0;
    //m00_axi_arqos <= 'h0;
    m00_axi_rready <= 1;

    // FIXME this could use bursts
    m01_axi_awlen <= 'h0; // 1 burst (1 write)
    m01_axi_awsize <= 'h2; //'h2; // 2^2 == 4 bytes
    m01_axi_awburst <= 'h0; // FIXED (non incrementing)
    m01_axi_awcache <= 'h0;
    m01_axi_awlock <= 'h0;
    m01_axi_awprot <= 'h0;
    //m01_axi_awqos <= 'h0;
    m01_axi_wlast <= 'h1;
    m01_axi_bready <= 'h1;
  end

  reg [9:0] videocap_x_sync;
  reg [9:0] vc_saving_line;
  reg [9:0] videocap_y_sync2;

  // pipeline stages for videocap save addr calculation
  reg [23:0] vc_saveaddr1;
  reg [31:0] vc_saveaddr2;
  reg [31:0] vc_saveaddr3;

  always @(posedge S_AXI_ACLK) begin
    // VIDEOCAP

    // pass interlace mode to video control block
    video_control_interlace <= vcap_interlace;

    videocap_pitch_sync <= videocap_pitch;

    //videocap_x_sync <= vcap_x;
    videocap_y_sync2 <= vcap_y[9:0];
    videocap_mode_sync <= videocap_mode;

`ifdef VARIANT_ZZ9500
    if (vcap_interlace)
      videocap_ymax_sync <= (vcap_ymax<<1)-(2*40);
    else
      videocap_ymax_sync <= vcap_ymax-36;

    // letterbox top and bottom to box out noisy lines
    if (videocap_y_sync2<videocap_ymax_sync && vcap_x_done) begin
      videocap_y_sync <= videocap_y_sync2;
    end
`else
    if (vcap_interlace)
      videocap_ymax_sync <= (vcap_ymax<<1);
    else
      videocap_ymax_sync <= vcap_ymax;

    if (vcap_x_done) begin
      videocap_y_sync <= videocap_y_sync2;
    end
`endif

    vc_saveaddr1 <= vc_saving_line*videocap_pitch_sync;
    //vc_saveaddr2 <= (vc_saveaddr1+videocap_save_x)<<2;
    //vc_saveaddr3 <= videocap_address+vc_saveaddr2;

    // FIXME
    if (videocap_save_line_done!=videocap_y_sync) begin
      vc_saving_line <= videocap_y_sync;
    end

    if (m01_axi_aresetn == 0) begin
      videocap_save_state <= 4;
      m01_axi_wvalid_out  <= 0;
      m01_axi_awvalid_out <= 0;
    end else begin

      // one-hot encoded
      case (videocap_save_state)
        4'h0: begin
          // initial state
          videocap_save_state <= 2;
          m01_axi_wvalid_out  <= 0;
          m01_axi_awvalid_out <= 0;
        end
        4'h1: begin
          if (m01_axi_wready) begin
            m01_axi_wvalid_out  <= 0;
            m01_axi_awvalid_out <= 0;
            if (videocap_mode_sync)
              videocap_save_state <= 2;
            else
              videocap_save_state <= 4;
          end
        end
        4'h2: begin
          // we shift left by 2 bits to scale from 1 pixel to 4 bytes
          m01_axi_awaddr_out  <= videocap_address + ((vc_saveaddr1 + videocap_save_x)<<2);
          m01_axi_wdata_out   <= vcap_rdata;

  `ifdef VARIANT_ZZ9500
          if (videocap_save_x >= videocap_pitch_sync-2) begin
  `else
          if (videocap_save_x >= videocap_pitch_sync) begin // 728 FIXME
  `endif
            videocap_save_line_done <= vc_saving_line;
            videocap_save_x <= 0;
          end else if (videocap_save_line_done != vc_saving_line) begin
            videocap_save_x <= videocap_save_x + 1'b1;

            m01_axi_awvalid_out <= 1;
            videocap_save_state <= 3;
          end
        end
        4'h3: begin
          if (m01_axi_awready) begin
            m01_axi_wvalid_out  <= 1;
            videocap_save_state <= 1;
          end
        end
        4'h4: begin
          // videocap is disabled, lets wait here
          if (videocap_mode_sync)
            videocap_save_state <= 0;

          m01_axi_wvalid_out  <= 0;
          m01_axi_awvalid_out <= 0;
        end
      endcase
    end
  end

  // -- main zorro fsm ---------------------------------------------
  always @(posedge S_AXI_ACLK) begin
    videocap_mode <= videocap_mode_in;

    if (/*z_cfgin_lo ||*/ z_reset) begin
      zorro_state <= RESET;
    end

    if ((axi_reg0 & SDK_CTRL_DOORBELL_CLEAR) != 0)
      sdk_doorbell_pending <= 0;
    if ((axi_reg0 & SDK_CTRL_IRQ_ACK_CLEAR) != 0)
      sdk_irq_ack_pending <= 0;
    
      case (zorro_state)

        COLD: begin
          zorro_state <= RESET;
        end

        RESET: begin
          dataout_enable <= 0;
          dataout <= 0;
          dataout_z3 <= 0;
          slaven <= 0;
          dtack <= 0;
          z_ovr <= 0;
          z_confout <= 0;
          z3_curpic <= 0;
          z3_confdone <= 0;
          zorro_ram_read_request <= 0;
          zorro_ram_write_request <= 0;
          zorro_ram_read_flag <= 0;
          zorro_ram_write_flag <= 0;
          sdk_doorbell_pending <= 0;
          sdk_irq_ack_pending <= 0;
          sdk_last_regwrite_addr <= 0;
          sdk_last_regwrite_data <= 0;
          sdk_last_regwrite_strobes <= 0;
          sdk_last_z3addr <= 0;
          sdk_doorbell_early_count <= 0;
          sdk_irq_ack_early_count <= 0;
          z3_ram_low <= 0;
          z3_ram_high <= 0;
          z3_fast_low <= 0;
          z3_fast_high <= 0;
          z3_fast_ddr_delta <= 0;
          z3_reg_low <= 0;
          z3_reg_high <= 0;
          reg_low <= 0;
          reg_high <= 0;
          ram_low <= 0;
          ram_high <= 0;

          if (!z_reset)
            zorro_state <= DECIDE_Z2_Z3;

          videocap_mode_in <= 1;
        end

        DECIDE_Z2_Z3: begin
`ifdef ZORRO2
          if (z2addr_autoconfig) begin
            zorro_state <= Z2_CONFIGURING;
          end
`endif

`ifdef ZORRO3
          zorro_state <= Z3_CONFIGURING;
`endif
        end

`ifdef ZORRO3
        Z3_AUTOCONF_READ_DLY: begin
          // wait for data to be latched out
          zorro_state <= Z3_AUTOCONF_READ_DLY2;
        end

        Z3_AUTOCONF_READ_DLY2: begin
          // wait for data to be latched out
          dtack <= 1;
          zorro_state <= Z3_DTACK;
        end

        Z3_AUTOCONF_READ: begin
          dataout_z3 <= 1;
          zorro_state <= Z3_AUTOCONF_READ_DLY;
          last_z3addr <= z3addr;

          case (z3addr[15:0])
            'h0000: begin
              if (!z3_curpic) begin
`ifdef VARIANT_AUTOBOOT
                data_z3_hi16 <= 'b1001_1111_1111_1111; // zorro 3 (10), no pool link (0), autoboot ROM yes (1)
`else
                data_z3_hi16 <= 'b1000_1111_1111_1111; // zorro 3 (10), no pool link (0), autoboot ROM no (0)
`endif
              end else begin
                data_z3_hi16 <= 'b1010_1111_1111_1111; // zorro 3 (10), pool link (1), autoboot ROM no (0)
              end
            end
            'h0100: begin
              if (!z3_curpic) begin
`ifndef VARIANT_FW20
                data_z3_hi16 <= 'b1011_1111_1111_1111; // next board related (1), 128MB
`else
                data_z3_hi16 <= 'b0010_1111_1111_1111; // FW20: next board unrelated (0), 64MB
`endif
              end else begin
                data_z3_hi16 <= 'b0100_1111_1111_1111; // next board unrelated (0), 256MB
              end
            end

            'h0004: data_z3_hi16 <= 'b1111_1111_1111_1111; // product number
            'h0104: begin
              if (!z3_curpic) begin
                data_z3_hi16 <= 'b1011_1111_1111_1111; // 4 for the ZZ9000 RTG PIC
              end else begin
                data_z3_hi16 <= 'b1010_1111_1111_1111; // 5 for the 256MB Z3 Fast
              end
            end

            'h0008: begin
              if (!z3_curpic) begin
                data_z3_hi16 <= 'b1000_1111_1111_1111; // flags inverted 1111 io,shutup,extension,reserved(1)
              end else begin
                data_z3_hi16 <= 'b1000_1111_1111_1111; // flags inverted 0111 io,shutup,extension,reserved(1)
              end
            end
            'h0108: data_z3_hi16 <= 'b1111_1111_1111_1111; // inverted zero

            'h000c: data_z3_hi16 <= 'b1111_1111_1111_1111; // reserved?
            'h010c: data_z3_hi16 <= 'b1111_1111_1111_1111; //

            'h0010: data_z3_hi16 <= 'b1001_1111_1111_1111; // manufacturer high byte inverted
            'h0110: data_z3_hi16 <= 'b0010_1111_1111_1111; //
            'h0014: data_z3_hi16 <= 'b1001_1111_1111_1111; // manufacturer low byte
            'h0114: data_z3_hi16 <= 'b0001_1111_1111_1111;

            /*'h0018: data_z3_hi16 <= 'b1111_1111_1111_1111; // serial 01 01 01 01
            'h0118: data_z3_hi16 <= 'b1110_1111_1111_1111; //
            'h001c: data_z3_hi16 <= 'b1111_1111_1111_1111; //
            'h011c: data_z3_hi16 <= 'b1110_1111_1111_1111; //
            'h0020: data_z3_hi16 <= 'b1111_1111_1111_1111; //
            'h0120: data_z3_hi16 <= 'b1110_1111_1111_1111; //
            'h0024: data_z3_hi16 <= 'b1111_1111_1111_1111; //
            'h0124: data_z3_hi16 <= 'b1110_1111_1111_1111; // */

`ifdef VARIANT_AUTOBOOT
            'h0028: data_z3_hi16 <= 'b1001_1111_1111_1111; // autoboot rom vector (er_InitDiagVec)
`else
            'h0028: data_z3_hi16 <= 'b1111_1111_1111_1111;
`endif
            'h0128: data_z3_hi16 <= 'b1111_1111_1111_1111; // = ~0x6000
            'h002c: data_z3_hi16 <= 'b1111_1111_1111_1111;
            'h012c: data_z3_hi16 <= 'b1111_1111_1111_1111;

            default: data_z3_hi16 <= 'b1111_1111_1111_1111;
          endcase
        end

        Z3_AUTOCONF_WRITE: begin
          if (z3_ds0||z3_ds1||z3_ds2||z3_ds3) begin
            dtack <= 1;
            zorro_state <= Z3_DTACK;
            casex (z3addr[15:0])
              'hXX44: begin
                if (!z3_curpic) begin
                  z3_ram_low[31:16] <= z3_din_high_s2;
                end else begin
                  z3_fast_low[31:16] <= z3_din_high_s2;
                end
                z3_confdone <= 1;
              end
              'hXX48: begin
              end
              'hXX4c: begin
                // shutup
                z3_confdone <= 1;
              end
            endcase
          end
        end

        Z3_CONFIGURING: begin
          if (z_cfgin && z3addr_autoconfig) begin
            if (zorro_read) begin
              // autoconfig ROM
              zorro_state <= Z3_AUTOCONF_READ;
              slaven <= 1;
            end else begin
              // write to autoconfig register
              zorro_state <= Z3_AUTOCONF_WRITE;
              slaven <= 1;
            end
          end else begin
            dataout_z3 <= 0;
            slaven <= 0;
            dtack <= 0;
            dtack_counter <= 0;
          end
        end

        Z3_DTACK: begin
          // see Z3_ENDCYCLE
          dtack_counter <= dtack_counter + 1'b1;
          if (dtack_counter >= dtack_timeout) begin
            dtack <= 0;
          end

          if (z3_fcs_state == 1) begin
            dtack <= 0;
            dataout_z3 <= 0;
            slaven <= 0;
            if (z3_confdone) begin
              zorro_state <= CONFIGURED;
            end else
              zorro_state <= Z3_CONFIGURING;
          end
        end
`endif

        CONFIGURED: begin
          if (ram_low) begin
            ram_high <= ram_low + `RAM_SIZE;
            reg_low <= ram_low + 'h1000;
            reg_high <= ram_low + 'h2000;
          end

`ifdef ZORRO3
          if (z3_ram_low) begin
            z3_ram_high  <= z3_ram_low + `Z3_SIZE_128MB;
            z3_reg_low   <= z3_ram_low + 'h1000;
            z3_reg_high  <= z3_ram_low + 'h2000;
          end

`ifdef VARIANT_Z3_FASTRAM
          if (z3_fast_low) begin
            z3_fast_high  <= z3_fast_low + `Z3_SIZE_256MB;
            z3_fast_ddr_delta <= z3_fast_low - (`Z3_FASTRAM_ARM_BASE - `ARM_MEMORY_START);
          end

          // issue #25: advertise the second (fast-RAM) PIC only once the
          // firmware has signalled the DDR window is ready (slv_reg6[0]). On a
          // cold boot the Zynq may still be booting from SD when a fast
          // accelerator (e.g. BFG9060) memory-tests Zorro III RAM; exposing the
          // fast-RAM board before its DDR backing is up makes that test mark it
          // "defective". Withholding the PIC until ready avoids the false
          // failure - it appears on the next autoconfig once ready. Warm resets
          // keep fastram_ready set, so they are unaffected.
          if (!z3_curpic && fastram_ready) begin
            z3_curpic <= 1'b1;
            z3_confdone <= 0;
            zorro_state <= Z3_CONFIGURING;
          end else begin
            z3_curpic <= 1'b0;
            zorro_state <= CONFIGURED_CLEAR;
          end
`else
          z3_curpic <= 1'b0;
          zorro_state <= CONFIGURED_CLEAR;
`endif

`else
// ZORRO 2
          zorro_state <= CONFIGURED_CLEAR;
`endif
        end

        CONFIGURED_CLEAR: begin
          z_confout <= 1;
`ifdef ZORRO3
          zorro_state <= Z3_IDLE;
`else
          zorro_state <= Z2_IDLE;
`endif
        end

        // ---------------------------------------------------------------------------------
`ifdef ZORRO2
        Z2_CONFIGURING: begin
          z_ovr <= 0;
          if (z2_addr_valid && z2addr_autoconfig && z_cfgin) begin
            if (z2_read) begin
              // read iospace 'he80000 (Autoconfig ROM)
              dataout_enable <= 1;
              dataout <= 1;
              slaven <= 1;

              case (z2_mapped_addr[7:0])
`ifdef VARIANT_AUTOBOOT
                8'h00: data_out <= 'b1101_1111_1111_1111; // zorro 2 (11), no pool (0) rom (1)
`else
                8'h00: data_out <= 'b1100_1111_1111_1111; // zorro 2 (11), no pool (0) no rom (0)
`endif
`ifdef VARIANT_2MB
                8'h02: data_out <= 'b0110_1111_1111_1111; // next board unrelated (0), 2mb (110)
`else
                8'h02: data_out <= 'b0111_1111_1111_1111; // next board unrelated (0), 4mb (111)
`endif
                8'h04: data_out <= 'b1111_1111_1111_1111; // product number
                8'h06: data_out <= 'b1100_1111_1111_1111; // (3)

                8'h08: data_out <= 'b0111_1111_1111_1111; // er_Type=0x80 (ERTF_DIAGVALID), er_Flags=0x00
                8'h0a: data_out <= 'b1111_1111_1111_1111; // er_Flags=0x00 (log=phys)

                8'h10: data_out <= 'b1001_1111_1111_1111; // manufacturer high byte inverted (02)
                8'h12: data_out <= 'b0010_1111_1111_1111; //
                8'h14: data_out <= 'b1001_1111_1111_1111; // manufacturer low byte (9a)
                8'h16: data_out <= 'b0001_1111_1111_1111;

                /*8'h18: data_out <= 'b1111_1111_1111_1111; // serial 01 01 01 01
                8'h1a: data_out <= 'b1110_1111_1111_1111; //
                8'h1c: data_out <= 'b1111_1111_1111_1111; //
                8'h1e: data_out <= 'b1110_1111_1111_1111; //
                8'h20: data_out <= 'b1111_1111_1111_1111; //
                8'h22: data_out <= 'b1110_1111_1111_1111; //
                8'h24: data_out <= 'b1111_1111_1111_1111; //
                8'h26: data_out <= 'b1110_1111_1111_1111; // */

`ifdef VARIANT_AUTOBOOT
                8'h28: data_out <= 'b1001_1111_1111_1111; // autoboot rom vector (er_InitDiagVec)
`else
                8'h28: data_out <= 'b1111_1111_1111_1111;
`endif
                8'h2a: data_out <= 'b1111_1111_1111_1111; // = ~0x6000
                8'h2c: data_out <= 'b1111_1111_1111_1111;
                8'h2e: data_out <= 'b1111_1111_1111_1111;

                8'h40: data_out <= 'b0000_0000_0000_0000; // interrupts (not inverted)
                8'h42: data_out <= 'b0000_0000_0000_0000; //

                default: data_out <= 'b1111_1111_1111_1111;
              endcase
            end else begin
              // write to autoconfig register
              if (z2_datastrobe_synced) begin
                case (z2_mapped_addr[7:0])
                  8'h48: begin
                    ram_low[31:24] <= 8'h0;
                    ram_low[23:20] <= zdata_in_sync[15:12];
                    ram_low[15:0] <= 16'h0;
                    zorro_state <= Z2_PRE_CONFIGURED; // configured
                  end
                  8'h4a: begin
                    ram_low[31:24] <= 8'h0;
                    ram_low[19:16] <= zdata_in_sync[15:12];
                    ram_low[15:0] <= 16'h0;
                  end

                  8'h4c: begin
                    zorro_state <= Z2_PRE_CONFIGURED; // configured, shut up
                  end
                endcase
              end
            end
          end else begin
            // no address match
            dataout <= 0;
            dataout_enable <= 0;
            slaven <= 0;
          end
        end
        Z2_PRE_CONFIGURED: begin
          if (!z2_addr_valid) begin
            zorro_state <= CONFIGURED;
          end
        end
        Z2_IDLE: begin
          if (z2_addr_valid) begin

            if (z2_write && z2addr_in_reg) begin
              // write to register
              dataout_enable <= 0;
              dataout <= 0;
              slaven <= 1;
              z_ovr <= 1;
              zaddr_regpart <= z2_mapped_addr[15:0];
              zorro_state <= Z2_REGWRITE;

            end else if (z2_read && z2addr_in_reg) begin
              // read from registers
              dataout_enable <= 1;
              dataout <= 1;
              data_out <= default_data;
              slaven <= 1;
              z_ovr <= 1;
              zaddr_regpart <= z2_mapped_addr[15:0];
              zorro_state <= Z2_REGREAD;

            end else if (z2_read && z2addr_in_ram) begin
              // read RAM
              // request ram access from arbiter
              last_addr <= z2_mapped_addr-ram_low; // differently done in z3
              data_out <= default_data;
              dataout_enable <= 1;
              dataout <= 1;
              slaven <= 1;
              z_ovr <= 1;
              zorro_state <= WAIT_READ;

            end else if (z2_write && z2addr_in_ram) begin
              // write RAM
              last_addr <= z2_mapped_addr-ram_low;
              dataout_enable <= 0;
              dataout <= 0;
              slaven <= 1;
              z_ovr <= 1;
              //count_writes <= count_writes + 1;
              zorro_state <= WAIT_WRITE;

            end else begin
              dataout <= 0;
              dataout_enable <= 0;
              slaven <= 0;
            end

          end else begin
            dataout <= 0;
            dataout_enable <= 0;
            slaven <= 0;
          end
        end
        Z2_REGWRITE: begin
          if (z2_datastrobe_synced) begin
            regdata_in <= zdata_in_sync;
            regwrite_addr <= zaddr_regpart;
            zorro_state <= REGWRITE;
          end
        end
        
        // =================
        
        WAIT_READ: begin
          if (last_addr<'h2000)
            // read via ARM
            zorro_state <= WAIT_READ3;
          else begin
            // read via AXI DMA
            if (last_addr>='ha000 && last_addr<'h10000)
              m00_axi_araddr  <= (`USB_BLOCK_STORAGE_ADDRESS - 32'ha000) + {last_addr[23:2],2'b00};
            else
            if (last_addr>='h8000 && last_addr<'hA000)
              m00_axi_araddr  <= (`TX_FRAME_ADDRESS - 32'h8000) + {last_addr[23:2],2'b00};
            else
            if (last_addr>='h2000 && last_addr<'h6000)
              m00_axi_araddr  <= (`RX_BACKLOG_ADDRESS - 32'h2000) + {last_addr[23:2],2'b00} + {eth_rx_frame_select, 11'h0}; // 11'h0 is FRAME_SIZE = 2048
            else
            if (last_addr>='h6000 && last_addr<'h8000)
              m00_axi_araddr  <= (`BOOT_ROM_ADDRESS - 32'h6000) + {last_addr[23:2],2'b00};
            else
              m00_axi_araddr  <= `ARM_MEMORY_START + {last_addr[23:2],2'b00};
  
            m00_axi_arvalid  <= 1;
            if (m00_axi_arready) begin
              zorro_state <= WAIT_READ2;
            end
            
          end
        end
        
        WAIT_READ2: begin
          m00_axi_arvalid <= 0;
          if (m00_axi_rvalid) begin
            zorro_state <= WAIT_READ2D;
            
            // le endian swap
            if (last_addr[1] == 1)
              data_out <= {m00_axi_rdata[23:16], m00_axi_rdata[31:24]};
            else
              data_out <= {m00_axi_rdata[7:0], m00_axi_rdata[15:8]};
          end
        end
        
        WAIT_READ2D: begin
          // delay state 3
          dtack_counter <= 0;
          dtack <= 1;
          zorro_state <= Z2_ENDCYCLE;
        end
        
        WAIT_READ3: begin
          // read via ARM
          zorro_ram_read_addr <= last_addr;
          zorro_ram_read_request <= 1;
          zorro_state <= WAIT_READ3B;
        end
        
        WAIT_READ3B: begin
          if (zorro_ram_read_flag) begin
            zorro_ram_read_request <= 0;

            data_out <= axi_reg1[15:0];
            zorro_state <= WAIT_READ3C;
          end
        end
        
        WAIT_READ3C: begin
          if (!zorro_ram_read_flag) begin
            dtack_counter <= 0;
            zorro_state <= WAIT_READ2D;
          end
        end
        
        // =================
        
        WAIT_WRITE: begin
          if (z2_datastrobe_synced) begin
            zorro_write_capture_bytes <= {~znUDS_sync[2],~znLDS_sync[2]}; // FIXME was 1
            zorro_write_capture_data <= zdata_in_sync;

            if (last_addr<'h10000)
              zorro_state <= WAIT_WRITE2;
            else
              zorro_state <= WAIT_WRITE_DMA_Z2;
          end
        end
        WAIT_WRITE2: begin
          // trace writes to arm test register
          //if (last_addr == 'h008c) begin
          //  debug_counter <= debug_counter + 1;
          //end
        
          zorro_ram_write_addr  <= last_addr;
          zorro_ram_write_bytes <= {2'b0,zorro_write_capture_bytes};
          zorro_ram_write_data  <= {16'b0,zorro_write_capture_data};
          zorro_ram_write_request <= 1;
          zorro_state <= Z2_WRITE_FINALIZE;
        end
        WAIT_WRITE_DMA_Z2: begin
          if (last_addr[1])
            m00_axi_wstrb_z3 <= {zorro_write_capture_bytes[0],zorro_write_capture_bytes[1],2'b0};
          else
            m00_axi_wstrb_z3 <= {2'b0,zorro_write_capture_bytes[0],zorro_write_capture_bytes[1]};

          m00_axi_awaddr_z3  <= (last_addr+`ARM_MEMORY_START)&'hfffffc;
          m00_axi_wdata_z3   <= {zorro_write_capture_data[7:0],zorro_write_capture_data[15:8],zorro_write_capture_data[7:0],zorro_write_capture_data[15:8]};
          m00_axi_awvalid_z3 <= 1;
          if (m00_axi_awready) begin // TODO wready?
            zorro_state <= WAIT_WRITE_DMA_Z2_FINALIZE;
          end
        end
        WAIT_WRITE_DMA_Z2_FINALIZE: begin
          m00_axi_awvalid_z3 <= 0;
          m00_axi_wvalid_z3 <= 1;
          if (m00_axi_wready) begin
            dtack <= 1;
            zorro_state <= Z2_ENDCYCLE;
          end
        end
        Z2_WRITE_FINALIZE: begin
          if (zorro_ram_write_flag) begin
            zorro_state <= Z2_WRITE_FINALIZE2;
            zorro_ram_write_request <= 0;
          end
        end
        
        Z2_WRITE_FINALIZE2: begin
          if (!zorro_ram_write_flag) begin
            zorro_state <= Z2_ENDCYCLE;
            dtack <= 1;
            //slaven <= 0;
          end
        end
        
        Z2_ENDCYCLE: begin
          m00_axi_wvalid_z3 <= 0;
          z_ovr <= 0;

          dtack_counter <= dtack_counter + 1'b1;
          if (dtack_counter >= dtack_timeout) begin
            dtack <= 0;
          end

          if (!z2_addr_valid) begin
            dtack <= 0;
            slaven <= 0;
            dataout_enable <= 0;
            dataout <= 0;
            zorro_state <= Z2_IDLE;
            dtack_counter <= 0;
          end
        end
        // 16bit reg read
        Z2_REGREAD_POST: begin
          if (zaddr_regpart[1]==1'b1)
            data_out <= rr_data[15:0];
          else
            data_out <= rr_data[31:16];
          dtack <= 1;
          zorro_state <= Z2_ENDCYCLE;
        end
        // relaxing the data pipeline a bit
        Z2_REGREAD: begin
          regread_addr <= zaddr_regpart;
          zorro_state <= REGREAD;
        end
`endif

`ifdef ZORRO3
        // =========================================================================
        // ZORRO 3
        // =========================================================================

        Z3_REGWRITE_PRE: begin
          if (z3_ds1) begin
            regdata_in <= z3_din_low_s2;
            z3addr_regpart <= (z3addr[15:0])|16'h2;
            zorro_state <= Z3_REGWRITE;
          end else if (z3_ds3) begin
            regdata_in <= z3_din_high_s2;
            z3addr_regpart <= z3addr[15:0];
            zorro_state <= Z3_REGWRITE;
          end
        end

        Z3_REGREAD_PRE: begin
          z3addr_regpart <= z3addr[15:0]; //|16'h2;
          zorro_state <= Z3_REGREAD;
          dataout_z3 <= 1;
        end

        Z3_IDLE: begin
          dtack_counter <= 0;

          if (z3_fcs_state==0) begin
            // falling edge of /FCS

            if (zorro_write && z3addr_in_reg) begin
              // FIXME doesn't support 32 bit access
              // write to register
              sdk_last_z3addr <= z3addr;
              case (z3addr[15:0] & SDK_REG_OFFSET_MASK)
                SDK_REG_DOORBELL: begin
                  sdk_doorbell_pending <= 1;
                  sdk_doorbell_early_count <= sdk_doorbell_early_count + 1;
                  dtack <= 1;
                  zorro_state <= Z3_ENDCYCLE;
                end
                SDK_REG_IRQ_ACK: begin
                  sdk_irq_ack_pending <= 1;
                  sdk_irq_ack_early_count <= sdk_irq_ack_early_count + 1;
                  dtack <= 1;
                  zorro_state <= Z3_ENDCYCLE;
                end
                default: begin
                  zorro_state <= Z3_REGWRITE_PRE;
                end
              endcase
              slaven <= 1;
            end else if (zorro_read && z3addr_in_reg) begin
              // read registers
              data_z3_hi16 <= default_data;
              data_z3_low16 <= default_data;
              zorro_state <= Z3_REGREAD_PRE;
              slaven <= 1;
            end else if (z3addr_in_ram && zorro_write) begin
              // write to memory
              slaven <= 1;

              zorro_state <= Z3_WRITE_PRE;
            end else if (z3addr_in_ram && zorro_read) begin
              // read from memory
              data_z3_hi16  <= default_data;
              data_z3_low16 <= default_data;
              slaven <= 1;

`ifndef VARIANT_FW20
              if (z3_mapped_addr<'h2000)
                zorro_state <= Z3_READ_UPPER;
              else
                zorro_state <= WAIT_READ_DMA_Z3;
`else
              zorro_state <= Z3_READ_UPPER;
`endif
            end else begin
              // address not recognized
              slaven <= 0;
            end

          end else begin
            // not in a cycle
            slaven <= 0;
          end
        end

        Z3_REGWRITE: begin
          regwrite_addr <= z3addr_regpart;
          zorro_state <= REGWRITE;
          dtack <= 1;
        end

        Z3_REGREAD: begin
          regread_addr <= z3addr_regpart;
          zorro_state <= REGREAD;
        end

        // 32bit reg read
        Z3_REGREAD_POST: begin
          data_z3_hi16  <= rr_data[31:16];
          data_z3_low16 <= rr_data[15:0];
          zorro_state <= Z3_ENDCYCLE;
          dtack <= 1;
        end

        Z3_READ_UPPER: begin
          zorro_state <= Z3_READ_DELAY1;
          last_z3addr <= z3_mapped_addr;
          zorro_ram_read_addr <= z3_mapped_addr;
          zorro_ram_read_bytes <= 4'b1111;
          zorro_ram_read_request <= 1;
          dataout_z3 <= 1; // enable data output

          // dummy read for debug
          /*dtack <= 1;
           data_z3_hi16 <= 'hffff;
           data_z3_low16 <= 'hffff;
           zorro_state <= Z3_ENDCYCLE;*/
        end

        Z3_READ_DELAY1: begin
          data_z3_hi16 <= axi_reg1[31:16];
          data_z3_low16 <= axi_reg1[15:0];

          if (zorro_ram_read_flag) begin
            zorro_ram_read_request <= 0; // acknowledge read request done
            zorro_state <= Z3_READ_DELAY2; // CHECK DELAY
          end
        end

        Z3_READ_DELAY2: begin
          if (!zorro_ram_read_flag) begin
            zorro_state <= Z3_ENDCYCLE;
            dtack <= 1;
            slaven <= 0;
          end
        end

        Z3_WRITE_PRE: begin
          if (z3_ds0||z3_ds1||z3_ds2||z3_ds3) begin
            zorro_state <= Z3_WRITE_PRE2;
          end
        end

        Z3_WRITE_PRE2: begin
          // FIXME DMA temporarily disabled for FW2.0
`ifdef VARIANT_FW20
          zorro_state <= Z3_WRITE_UPPER;
`else
          if (z3_mapped_addr<'h2000)
            zorro_state <= Z3_WRITE_UPPER;
          else
            zorro_state <= WAIT_WRITE_DMA_Z3;
`endif
        end

        Z3_WRITE_UPPER: begin
          // trace writes to arm test register
          //if (z3_mapped_addr == 'h008c) begin
          //  debug_counter <= debug_counter + 1;
          //end
        
          last_z3addr <= z3_mapped_addr;
          zorro_ram_write_addr  <= z3_mapped_addr;
          zorro_ram_write_bytes <= {z3_ds3,z3_ds2,z3_ds1,z3_ds0};
          zorro_ram_write_data  <= {z3_din_high_s2,z3_din_low_s2};
          zorro_ram_write_request <= 1;

          zorro_state <= Z3_WRITE_FINALIZE;
        end

        Z3_WRITE_FINALIZE: begin
          if (zorro_ram_write_flag) begin
            zorro_ram_write_request <= 0; // acknowledge write request done
            zorro_state <= Z3_WRITE_FINALIZE2;
          end
        end
        
        Z3_WRITE_FINALIZE2: begin
          if (!zorro_ram_write_flag) begin
            zorro_state <= Z3_ENDCYCLE;
            dtack <= 1;
            slaven <= 0;
          end
        end

        WAIT_READ_DMA_Z3: begin
          if (z3_mapped_addr>='ha000 && z3_mapped_addr<'h10000)
            m00_axi_araddr  <= (`USB_BLOCK_STORAGE_ADDRESS - 32'ha000) + z3_mapped_addr;
          else
          if (z3_mapped_addr>='h8000 && z3_mapped_addr<'hA000)
            m00_axi_araddr  <= (`TX_FRAME_ADDRESS - 32'h8000) + z3_mapped_addr;
          else
          if (z3_mapped_addr>='h2000 && z3_mapped_addr<'h6000)
            m00_axi_araddr  <= (`RX_BACKLOG_ADDRESS - 32'h2000) + z3_mapped_addr + {eth_rx_frame_select, 11'h0}; // 11'h0 is FRAME_SIZE = 2048
          else
          if (z3_mapped_addr>='h6000 && z3_mapped_addr<'h8000)
            m00_axi_araddr  <= (`BOOT_ROM_ADDRESS - 32'h6000) + z3_mapped_addr;
          else
            m00_axi_araddr  <= `ARM_MEMORY_START + (z3_mapped_addr/*&32'hfffffffc*/); // max 256MB

          m00_axi_arvalid  <= 1;
          if (m00_axi_arready) begin
            zorro_state <= WAIT_READ_DMA_Z3B;
          end
        end

        WAIT_READ_DMA_Z3B: begin
          m00_axi_arvalid <= 0;
          if (m00_axi_rvalid) begin
            zorro_state <= Z3_ENDCYCLE;
            data_z3_hi16 <= {m00_axi_rdata[7:0], m00_axi_rdata[15:8]};
            data_z3_low16 <= {m00_axi_rdata[23:16], m00_axi_rdata[31:24]};
            dataout_z3 <= 1; // enable data output
            dtack <= 1;
          end
        end

        WAIT_WRITE_DMA_Z3: begin
          m00_axi_wstrb_z3   <= {z3_ds0, z3_ds1, z3_ds2, z3_ds3};
          if ( (z3_mapped_addr>='hA000)&&(z3_mapped_addr<'h10000) )
            m00_axi_awaddr_z3 <= (`USB_BLOCK_STORAGE_ADDRESS - 32'hA000) + z3_mapped_addr;
          else
          if ( (z3_mapped_addr>='h8000)&&(z3_mapped_addr<'hA000) )
            m00_axi_awaddr_z3 <= (`TX_FRAME_ADDRESS - 32'h8000) + z3_mapped_addr;
          else
          if ( (z3_mapped_addr>='h2000)&&(z3_mapped_addr<'h8000) ) // this is marked in main.c as "FIXME remove"
            m00_axi_awaddr_z3 <= (`RX_FRAME_ADDRESS - 32'h2000) + z3_mapped_addr;
          else
            m00_axi_awaddr_z3  <= `ARM_MEMORY_START + (z3_mapped_addr/*&32'hfffffffc*/); // max 256MB
          m00_axi_wdata_z3   <= {z3_din_low_s2[7:0], z3_din_low_s2[15:8], z3_din_high_s2[7:0], z3_din_high_s2[15:8]};

          m00_axi_awvalid_z3  <= 1;
          if (m00_axi_awready) begin
            zorro_state <= WAIT_WRITE_DMA_Z3B;
          end
        end

        WAIT_WRITE_DMA_Z3B: begin
          dtack <= 1;
          m00_axi_awvalid_z3 <= 0;
          m00_axi_wvalid_z3 <= 1;
          if (m00_axi_wready) begin
            zorro_state <= WAIT_WRITE_DMA_Z3C;
          end
        end

        // not sure if this extra state is needed actually
        WAIT_WRITE_DMA_Z3C: begin
          m00_axi_wvalid_z3 <= 0;
          zorro_state <= Z3_ENDCYCLE;
        end

        Z3_ENDCYCLE: begin
          dtack <= 1;

          // we're timing out or own dtack here. because of a zorro
          // bug / subtlety, dtack can be sampled incorrectly to "hang over"
          // into the next amiga zorro cycle.
          // this is because we have a long rise time on our DTACK
          // output/1k pullup.
          dtack_counter <= dtack_counter + 1'b1;
          if (dtack_counter >= dtack_timeout) begin
            dtack <= 0;
          end

          if (z3_fcs_state==1) begin
            dtack <= 0;
            slaven <= 0;
            dataout_z3 <= 0;
            zorro_state <= Z3_IDLE;
          end
        end
`endif

        // FIXME why is there no dataout time on REGREAD? (see memory reads)
        // now fixed for Z3, still pending for Z2
        REGREAD: begin
          // TODO split up into z3/z2
`ifdef ZORRO3
          zorro_state <= Z3_REGREAD_POST;
`else
          zorro_state <= Z2_REGREAD_POST;
`endif

          case (regread_addr & SDK_REG_OFFSET_MASK)
            SDK_REG_MAGIC: begin
              rr_data[31:16] <= SDK_REG_MAGIC_VALUE;
              rr_data[15:0]  <= SDK_REG_VERSION;
            end
            SDK_REG_VERSION_OFFS: begin
              rr_data[31:16] <= SDK_REG_VERSION;
              rr_data[15:0]  <= SDK_REG_VERSION;
            end
            SDK_REG_MAILBOX_HI,
            SDK_REG_MAILBOX_LO: begin
              rr_data <= SDK_MAILBOX_ARM_ADDRESS;
            end
            SDK_REG_DOORBELL,
            SDK_REG_STATUS: begin
              rr_data[31:16] <= 16'h0000;
              rr_data[15:0]  <= {14'h0000, sdk_irq_ack_pending,
                                  sdk_doorbell_pending};
            end
            SDK_REG_DIAG_WRITE,
            SDK_REG_DIAG_WRITE_LO: begin
              rr_data[31:16] <= sdk_last_regwrite_addr;
              rr_data[15:0]  <= {8'h00, sdk_last_regwrite_strobes,
                                  sdk_irq_ack_pending, sdk_doorbell_pending,
                                  2'b00};
            end
            SDK_REG_DIAG_DATA,
            SDK_REG_DIAG_DATA_LO: begin
              rr_data[31:16] <= sdk_last_regwrite_data;
              rr_data[15:0]  <= {sdk_irq_ack_early_count[7:0],
                                  sdk_doorbell_early_count[7:0]};
            end
            SDK_REG_DIAG_Z3ADDR,
            SDK_REG_DIAG_Z3ADDR_LO: begin
              rr_data <= sdk_last_z3addr;
            end
            default: begin
              case (regread_addr&'hff)
                /*'h00: begin
                 rr_data <= video_control_data;
                end
                 'h04: begin
                 rr_data <= video_control_op;
                end*/
                'h00: begin
                  rr_data <= video_control_vblank << 16;
                end
                'h30: begin
                  rr_data <= debug_counter << 16;
                end
                default: begin
                  rr_data[31:16] <= REVISION;
                  rr_data[15:0]  <= REVISION;
                end
              endcase
            end
          endcase
        end

        REGWRITE: begin
`ifdef ZORRO3
          zorro_state <= Z3_ENDCYCLE;
`else
          dtack <= 1;
          zorro_state <= Z2_ENDCYCLE;
`endif

          case (regwrite_addr & SDK_REG_OFFSET_MASK)
            SDK_REG_DOORBELL,
            SDK_REG_DOORBELL_Z3_LO: sdk_doorbell_pending <= 1;
            SDK_REG_IRQ_ACK,
            SDK_REG_IRQ_ACK_Z3_LO: sdk_irq_ack_pending <= 1;
            default: begin
              case (regwrite_addr&'hff)
                'h00: video_control_data_zorro[31:16] <= regdata_in[15:0];
                'h02: video_control_data_zorro[15:0]  <= regdata_in[15:0];
                'h04: video_control_op_zorro[7:0]     <= regdata_in[7:0]; // FIXME
                'h06: videocap_mode_in <= regdata_in[0];
                'h08: scanline_intensity  <= regdata_in[7:0];
                'h0A: scanline_intensity2 <= regdata_in[7:0];
                'h0C: scanline_width      <= regdata_in[1:0];
                'h0E: scanline_parity     <= regdata_in[0];
                //'h08: E7M_RESET <= regdata_in[0];
                //'h0a: E7M_PWRDWN <= regdata_in[0];
                'h10: videocap_address[31:16] <= regdata_in[15:0];
                'h12: videocap_address[15:0] <= regdata_in[15:0];
                'h14: videocap_pitch <= regdata_in[15:0];
                'h20: if (regdata_in[5:0]>0) dtack_timeout <= regdata_in[5:0];
                //'h24: dataout_time[7:0]     <= regdata_in[7:0];
                'h24: zorro_interrupt_len <= regdata_in[7:0];
                //'h10: E7M_PSINCDEC <= regdata_in[0];
                //'h12: E7M_PSEN     <= regdata_in[0];
                //'h30: debug_counter <= debug_counter + 1;
                'h34: debug_counter <= 0;
              endcase
            end
          endcase

          sdk_last_regwrite_addr <= regwrite_addr;
          sdk_last_regwrite_data <= regdata_in;
          sdk_last_regwrite_strobes <= {z3_ds3, z3_ds2, z3_ds1, z3_ds0};
        end
      endcase

    // PSEN reset
    //if (E7M_PSEN==1'b1) E7M_PSEN <= 1'b0;

    // ARM video control
    if (axi_reg2[31]==1'b1) begin
      video_control_data_axi <= axi_reg3[31:0];
      video_control_op_axi   <= axi_reg2[7:0];
      video_control_axi <= 1;
    end else
      video_control_axi <= 0;

    // IRQ line to amiga
    if (axi_reg5[1] == 1) begin
      zorro_interrupt_pulse <= axi_reg5[0];
      //if (zorro_interrupt_req == 0 && axi_reg5[0] == 1) begin
        // start IRQ pulse only once on rising edge of zorro_interrupt_req (axi_reg5[0])
      //  zorro_interrupt_pulse <= zorro_interrupt_len;
      //end
      //zorro_interrupt_req <= axi_reg5[0];
      
      //if (zorro_interrupt_pulse == 'hff && axi_reg5[0] == 0)
      //  zorro_interrupt_pulse <= 0;
    end
    
    //if (zorro_interrupt_pulse > 0 && zorro_interrupt_pulse < 'hff)
    //  zorro_interrupt_pulse <= zorro_interrupt_pulse - 1'b1;
    
    if (axi_reg5[3] == 1)
      zz9000ax_reset_out <= axi_reg5[2];

    // read / write request acknowledged by ARM
    zorro_ram_read_flag  <= axi_reg0[30];
    zorro_ram_write_flag <= axi_reg0[31];

    axi_reg0 <= slv_reg0;
    axi_reg1 <= slv_reg1;
    axi_reg2 <= slv_reg2; // ARM video control
    axi_reg3 <= slv_reg3; // ARM video control
    eth_rx_frame_select <= slv_reg4;
    axi_reg5 <= slv_reg5; // Amiga IRQ
    fastram_ready <= slv_reg6[0]; // issue #25: gate Z3 fast-RAM PIC on firmware readiness

    if (video_control_axi) begin
      video_control_data <= video_control_data_axi;
      video_control_op   <= video_control_op_axi;
    end else begin
      video_control_data <= video_control_data_zorro;
      video_control_op   <= video_control_op_zorro;
    end

    video_control_data_out <= video_control_data;
    video_control_op_out   <= video_control_op;
    video_control_vblank   <= video_control_vblank_in[0];
    video_control_hblank   <= video_control_vblank_in[1];
    video_control_interlace_out <= video_control_interlace;
    scanline_intensity_out  <= scanline_intensity;
    scanline_intensity2_out <= scanline_intensity2;
    scanline_width_out      <= scanline_width;
    scanline_parity_out     <= scanline_parity;

    // snoop the screen width for correct capture pitch
    if (video_control_op == 2) begin
      // OP_DIMENSIONS = 2
      videocap_pitch <= video_control_data[11:0];
    end

    // snoop scanline settings sent over the video-control op path
    // (MNTVF_OP_SCANLINES). the video formatter ignores this op; it
    // exists so the ARM can apply ZZ9000.CFG scanline options at cold
    // boot. the Zorro-side registers at 'h08-'h0E keep working and
    // simply overwrite these values (last write wins).
    if (video_control_op == 20) begin
      // OP_SCANLINES = 20
      scanline_width  <= video_control_data[1:0];
      scanline_parity <= video_control_data[2];
    end

    // Snoop videocap sampler control (MNTVF_OP_VIDEOCAP). The video
    // formatter declares op 16 as ignored; it exists so the ARM can apply
    // ZZ9000.CFG videocap options at cold boot.
    if (video_control_op == 16) begin
      // OP_VIDEOCAP = 16
      videocap_sample_mode <= video_control_data[1:0];
      videocap_full_width  <= video_control_data[2];
      videocap_crop_h      <= video_control_data[15:4];
      videocap_crop_v      <= video_control_data[27:16];
    end

    out_reg0 <= ZORRO3 ? last_z3addr : last_addr;
    out_reg1 <= zorro_ram_write_data;
    out_reg2 <= last_z3addr;
    // Status: [24] interlace, [23] videocap, [22] NTSC, [21] vblank,
    // [20] hblank, [19] SDK doorbell, [18] SDK IRQ ack, [17] SuperHires.
    out_reg3 <= {zorro_ram_write_request, zorro_ram_read_request, zorro_ram_write_bytes, ZORRO3,
                video_control_interlace, videocap_mode, vcap_ntsc, video_control_vblank, video_control_hblank,
                sdk_doorbell_pending, sdk_irq_ack_pending, vcap_shres, 9'b0, zorro_state};
  end

  assign slv_reg_rden = axi_arready & S_AXI_ARVALID & ~axi_rvalid;
  always @(*)
    begin
      // Address decoding for reading registers
      case ( axi_araddr[ADDR_LSB+OPT_MEM_ADDR_BITS:ADDR_LSB] )
        3'h0   : reg_data_out <= out_reg0;
        3'h1   : reg_data_out <= out_reg1;
        3'h2   : reg_data_out <= out_reg2;
        3'h3   : reg_data_out <= out_reg3;
        default : reg_data_out <= 'h0;
      endcase
    end

endmodule
