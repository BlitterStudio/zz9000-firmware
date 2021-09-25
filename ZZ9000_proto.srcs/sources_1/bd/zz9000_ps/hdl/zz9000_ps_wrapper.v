//Copyright 1986-2018 Xilinx, Inc. All Rights Reserved.
//--------------------------------------------------------------------------------
//Tool Version: Vivado v.2018.3 (lin64) Build 2405991 Thu Dec  6 23:36:41 MST 2018
//Date        : Thu Sep 23 23:36:32 2021
//Host        : mntmn-i9 running 64-bit Debian GNU/Linux 11 (bullseye)
//Command     : generate_target zz9000_ps_wrapper.bd
//Design      : zz9000_ps_wrapper
//Purpose     : IP block netlist
//--------------------------------------------------------------------------------
`timescale 1 ps / 1 ps

module zz9000_ps_wrapper
   (DDR_addr,
    DDR_ba,
    DDR_cas_n,
    DDR_ck_n,
    DDR_ck_p,
    DDR_cke,
    DDR_cs_n,
    DDR_dm,
    DDR_dq,
    DDR_dqs_n,
    DDR_dqs_p,
    DDR_odt,
    DDR_ras_n,
    DDR_reset_n,
    DDR_we_n,
    FIXED_IO_ddr_vrn,
    FIXED_IO_ddr_vrp,
    FIXED_IO_mio,
    FIXED_IO_ps_clk,
    FIXED_IO_ps_porb,
    FIXED_IO_ps_srstb,
    I2SO_BCLK,
    I2SO_D0,
    I2SO_LRCLK,
    I2SO_MCLK,
    VCAP_B0,
    VCAP_B1,
    VCAP_B2,
    VCAP_B3,
    VCAP_B4,
    VCAP_B5,
    VCAP_B6,
    VCAP_B7,
    VCAP_G0,
    VCAP_G1,
    VCAP_G2,
    VCAP_G3,
    VCAP_G4,
    VCAP_G5,
    VCAP_G6,
    VCAP_G7,
    VCAP_HSYNC,
    VCAP_R0,
    VCAP_R1,
    VCAP_R2,
    VCAP_R3,
    VCAP_R4,
    VCAP_R5,
    VCAP_R6,
    VCAP_R7,
    VCAP_VSYNC,
    VGA_B,
    VGA_DE,
    VGA_G,
    VGA_HS,
    VGA_PCLK,
    VGA_R,
    VGA_VS,
    ZORRO_ADDR,
    ZORRO_ADDRDIR,
    ZORRO_ADDRDIR2,
    ZORRO_C28D,
    ZORRO_DATA,
    ZORRO_DATADIR,
    ZORRO_DOE,
    ZORRO_E7M,
    ZORRO_INT6,
    ZORRO_NBGN,
    ZORRO_NBRN,
    ZORRO_NCCS,
    ZORRO_NCFGIN,
    ZORRO_NCFGOUT,
    ZORRO_NCINH,
    ZORRO_NDS0,
    ZORRO_NDS1,
    ZORRO_NDTACK,
    ZORRO_NFCS,
    ZORRO_NIORST,
    ZORRO_NLDS,
    ZORRO_NSLAVE,
    ZORRO_NUDS,
    ZORRO_READ);
  inout [14:0]DDR_addr;
  inout [2:0]DDR_ba;
  inout DDR_cas_n;
  inout DDR_ck_n;
  inout DDR_ck_p;
  inout DDR_cke;
  inout DDR_cs_n;
  inout [3:0]DDR_dm;
  inout [31:0]DDR_dq;
  inout [3:0]DDR_dqs_n;
  inout [3:0]DDR_dqs_p;
  inout DDR_odt;
  inout DDR_ras_n;
  inout DDR_reset_n;
  inout DDR_we_n;
  inout FIXED_IO_ddr_vrn;
  inout FIXED_IO_ddr_vrp;
  inout [53:0]FIXED_IO_mio;
  inout FIXED_IO_ps_clk;
  inout FIXED_IO_ps_porb;
  inout FIXED_IO_ps_srstb;
  input I2SO_BCLK;
  output I2SO_D0;
  input I2SO_LRCLK;
  output I2SO_MCLK;
  input VCAP_B0;
  input VCAP_B1;
  input VCAP_B2;
  input VCAP_B3;
  input VCAP_B4;
  input VCAP_B5;
  input VCAP_B6;
  input VCAP_B7;
  input VCAP_G0;
  input VCAP_G1;
  input VCAP_G2;
  input VCAP_G3;
  input VCAP_G4;
  input VCAP_G5;
  input VCAP_G6;
  input VCAP_G7;
  input VCAP_HSYNC;
  input VCAP_R0;
  input VCAP_R1;
  input VCAP_R2;
  input VCAP_R3;
  input VCAP_R4;
  input VCAP_R5;
  input VCAP_R6;
  input VCAP_R7;
  input VCAP_VSYNC;
  output [7:0]VGA_B;
  output VGA_DE;
  output [7:0]VGA_G;
  output VGA_HS;
  output VGA_PCLK;
  output [7:0]VGA_R;
  output VGA_VS;
  inout [22:0]ZORRO_ADDR;
  output ZORRO_ADDRDIR;
  output ZORRO_ADDRDIR2;
  input ZORRO_C28D;
  inout [15:0]ZORRO_DATA;
  output ZORRO_DATADIR;
  input ZORRO_DOE;
  input ZORRO_E7M;
  output ZORRO_INT6;
  input ZORRO_NBGN;
  output ZORRO_NBRN;
  input ZORRO_NCCS;
  input ZORRO_NCFGIN;
  output ZORRO_NCFGOUT;
  output ZORRO_NCINH;
  input ZORRO_NDS0;
  input ZORRO_NDS1;
  output ZORRO_NDTACK;
  input ZORRO_NFCS;
  input ZORRO_NIORST;
  input ZORRO_NLDS;
  output ZORRO_NSLAVE;
  input ZORRO_NUDS;
  input ZORRO_READ;

  wire [14:0]DDR_addr;
  wire [2:0]DDR_ba;
  wire DDR_cas_n;
  wire DDR_ck_n;
  wire DDR_ck_p;
  wire DDR_cke;
  wire DDR_cs_n;
  wire [3:0]DDR_dm;
  wire [31:0]DDR_dq;
  wire [3:0]DDR_dqs_n;
  wire [3:0]DDR_dqs_p;
  wire DDR_odt;
  wire DDR_ras_n;
  wire DDR_reset_n;
  wire DDR_we_n;
  wire FIXED_IO_ddr_vrn;
  wire FIXED_IO_ddr_vrp;
  wire [53:0]FIXED_IO_mio;
  wire FIXED_IO_ps_clk;
  wire FIXED_IO_ps_porb;
  wire FIXED_IO_ps_srstb;
  wire I2SO_BCLK;
  wire I2SO_D0;
  wire I2SO_LRCLK;
  wire I2SO_MCLK;
  wire VCAP_B0;
  wire VCAP_B1;
  wire VCAP_B2;
  wire VCAP_B3;
  wire VCAP_B4;
  wire VCAP_B5;
  wire VCAP_B6;
  wire VCAP_B7;
  wire VCAP_G0;
  wire VCAP_G1;
  wire VCAP_G2;
  wire VCAP_G3;
  wire VCAP_G4;
  wire VCAP_G5;
  wire VCAP_G6;
  wire VCAP_G7;
  wire VCAP_HSYNC;
  wire VCAP_R0;
  wire VCAP_R1;
  wire VCAP_R2;
  wire VCAP_R3;
  wire VCAP_R4;
  wire VCAP_R5;
  wire VCAP_R6;
  wire VCAP_R7;
  wire VCAP_VSYNC;
  wire [7:0]VGA_B;
  wire VGA_DE;
  wire [7:0]VGA_G;
  wire VGA_HS;
  wire VGA_PCLK;
  wire [7:0]VGA_R;
  wire VGA_VS;
  wire [22:0]ZORRO_ADDR;
  wire ZORRO_ADDRDIR;
  wire ZORRO_ADDRDIR2;
  wire ZORRO_C28D;
  wire [15:0]ZORRO_DATA;
  wire ZORRO_DATADIR;
  wire ZORRO_DOE;
  wire ZORRO_E7M;
  wire ZORRO_INT6;
  wire ZORRO_NBGN;
  wire ZORRO_NBRN;
  wire ZORRO_NCCS;
  wire ZORRO_NCFGIN;
  wire ZORRO_NCFGOUT;
  wire ZORRO_NCINH;
  wire ZORRO_NDS0;
  wire ZORRO_NDS1;
  wire ZORRO_NDTACK;
  wire ZORRO_NFCS;
  wire ZORRO_NIORST;
  wire ZORRO_NLDS;
  wire ZORRO_NSLAVE;
  wire ZORRO_NUDS;
  wire ZORRO_READ;

  zz9000_ps zz9000_ps_i
       (.DDR_addr(DDR_addr),
        .DDR_ba(DDR_ba),
        .DDR_cas_n(DDR_cas_n),
        .DDR_ck_n(DDR_ck_n),
        .DDR_ck_p(DDR_ck_p),
        .DDR_cke(DDR_cke),
        .DDR_cs_n(DDR_cs_n),
        .DDR_dm(DDR_dm),
        .DDR_dq(DDR_dq),
        .DDR_dqs_n(DDR_dqs_n),
        .DDR_dqs_p(DDR_dqs_p),
        .DDR_odt(DDR_odt),
        .DDR_ras_n(DDR_ras_n),
        .DDR_reset_n(DDR_reset_n),
        .DDR_we_n(DDR_we_n),
        .FIXED_IO_ddr_vrn(FIXED_IO_ddr_vrn),
        .FIXED_IO_ddr_vrp(FIXED_IO_ddr_vrp),
        .FIXED_IO_mio(FIXED_IO_mio),
        .FIXED_IO_ps_clk(FIXED_IO_ps_clk),
        .FIXED_IO_ps_porb(FIXED_IO_ps_porb),
        .FIXED_IO_ps_srstb(FIXED_IO_ps_srstb),
        .I2SO_BCLK(I2SO_BCLK),
        .I2SO_D0(I2SO_D0),
        .I2SO_LRCLK(I2SO_LRCLK),
        .I2SO_MCLK(I2SO_MCLK),
        .VCAP_B0(VCAP_B0),
        .VCAP_B1(VCAP_B1),
        .VCAP_B2(VCAP_B2),
        .VCAP_B3(VCAP_B3),
        .VCAP_B4(VCAP_B4),
        .VCAP_B5(VCAP_B5),
        .VCAP_B6(VCAP_B6),
        .VCAP_B7(VCAP_B7),
        .VCAP_G0(VCAP_G0),
        .VCAP_G1(VCAP_G1),
        .VCAP_G2(VCAP_G2),
        .VCAP_G3(VCAP_G3),
        .VCAP_G4(VCAP_G4),
        .VCAP_G5(VCAP_G5),
        .VCAP_G6(VCAP_G6),
        .VCAP_G7(VCAP_G7),
        .VCAP_HSYNC(VCAP_HSYNC),
        .VCAP_R0(VCAP_R0),
        .VCAP_R1(VCAP_R1),
        .VCAP_R2(VCAP_R2),
        .VCAP_R3(VCAP_R3),
        .VCAP_R4(VCAP_R4),
        .VCAP_R5(VCAP_R5),
        .VCAP_R6(VCAP_R6),
        .VCAP_R7(VCAP_R7),
        .VCAP_VSYNC(VCAP_VSYNC),
        .VGA_B(VGA_B),
        .VGA_DE(VGA_DE),
        .VGA_G(VGA_G),
        .VGA_HS(VGA_HS),
        .VGA_PCLK(VGA_PCLK),
        .VGA_R(VGA_R),
        .VGA_VS(VGA_VS),
        .ZORRO_ADDR(ZORRO_ADDR),
        .ZORRO_ADDRDIR(ZORRO_ADDRDIR),
        .ZORRO_ADDRDIR2(ZORRO_ADDRDIR2),
        .ZORRO_C28D(ZORRO_C28D),
        .ZORRO_DATA(ZORRO_DATA),
        .ZORRO_DATADIR(ZORRO_DATADIR),
        .ZORRO_DOE(ZORRO_DOE),
        .ZORRO_E7M(ZORRO_E7M),
        .ZORRO_INT6(ZORRO_INT6),
        .ZORRO_NBGN(ZORRO_NBGN),
        .ZORRO_NBRN(ZORRO_NBRN),
        .ZORRO_NCCS(ZORRO_NCCS),
        .ZORRO_NCFGIN(ZORRO_NCFGIN),
        .ZORRO_NCFGOUT(ZORRO_NCFGOUT),
        .ZORRO_NCINH(ZORRO_NCINH),
        .ZORRO_NDS0(ZORRO_NDS0),
        .ZORRO_NDS1(ZORRO_NDS1),
        .ZORRO_NDTACK(ZORRO_NDTACK),
        .ZORRO_NFCS(ZORRO_NFCS),
        .ZORRO_NIORST(ZORRO_NIORST),
        .ZORRO_NLDS(ZORRO_NLDS),
        .ZORRO_NSLAVE(ZORRO_NSLAVE),
        .ZORRO_NUDS(ZORRO_NUDS),
        .ZORRO_READ(ZORRO_READ));
endmodule
