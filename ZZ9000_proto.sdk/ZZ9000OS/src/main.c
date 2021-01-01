/*
 * MNT ZZ9000 Amiga Graphics and Coprocessor Card Operating System (ZZ9000OS)
 *
 * Copyright (C) 2019, Lukas F. Hartmann <lukas@mntre.com>
 *                     MNT Research GmbH, Berlin
 *                     https://mntre.com
 *
 * More Info: https://mntre.com/zz9000
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * GNU General Public License v3.0 or later
 *
 * https://spdx.org/licenses/GPL-3.0-or-later.html
 *
 */

#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <math.h>
#include "platform.h"
#include "xil_printf.h"
#include "xparameters.h"
#include "xil_io.h"
#include "xscugic.h"

#include "xiicps.h"
#include "sleep.h"
#include "xaxivdma.h"
#include "xil_cache.h"
#include "xclk_wiz.h"
#include "xil_exception.h"
#include "xadcps.h"
#include "xtime_l.h"

#include "gfx.h"
#include "ethernet.h"
#include "usb.h"
#include "xgpiops.h"

#include "xil_misc_psreset_api.h"

#include "zz_regs.h"
#include "zz_video_modes.h"

typedef u8 uint8_t;

#define A9_CPU_RST_CTRL		(XSLCR_BASEADDR + 0x244)
#define A9_RST1_MASK 		0x00000002
#define A9_CLKSTOP1_MASK	0x00000020

#define XSLCR_LOCK_ADDR		(XSLCR_BASEADDR + 0x4)
#define XSLCR_LOCK_CODE		0x0000767B

#define IIC_DEVICE_ID	XPAR_XIICPS_0_DEVICE_ID
#define VDMA_DEVICE_ID	XPAR_AXIVDMA_0_DEVICE_ID
#define HDMI_I2C_ADDR 	0x3b
#define IIC_SCLK_RATE	400000
#define GPIO_DEVICE_ID		XPAR_XGPIOPS_0_DEVICE_ID

#define I2C_PAUSE 10

// I2C controller instance
XIicPs Iic;

// XADC adc converter instance
XAdcPs Xadc;

int xadc_init() {
	printf("xadc_init()...");
	XAdcPs_Config* cfg;
	cfg = XAdcPs_LookupConfig(XPAR_XADCPS_0_DEVICE_ID);
	XAdcPs_CfgInitialize(&Xadc, cfg, cfg->BaseAddress);
	int status = XAdcPs_SelfTest(&Xadc);
	printf("xadc_init() done: %d", status);
	return status;
}

float xadc_get_temperature() {
	u16 raw = XAdcPs_GetAdcData(&Xadc, XADCPS_CH_TEMP);
	return XAdcPs_RawToTemperature(raw);
}

float xadc_get_aux_voltage() {
	u16 raw = XAdcPs_GetAdcData(&Xadc, XADCPS_CH_VCCAUX);
	return XAdcPs_RawToVoltage(raw);
}

float xadc_get_int_voltage() {
	u16 raw = XAdcPs_GetAdcData(&Xadc, XADCPS_CH_VCCINT);
	return XAdcPs_RawToVoltage(raw);
}

// This is the absolute offset in ZZ9000 RAM for the "framebuffer transfer register",
// which can be replaced by the DMA acceleration functionality entirely, but some
// software still relies on this legacy register.
unsigned int cur_mem_offset = 0x3500000;

int hdmi_ctrl_write_byte(u8 addr, u8 value) {
	u8 buffer[2];
	buffer[0] = addr;
	buffer[1] = value;
	int status;

	while (XIicPs_BusIsBusy(&Iic)) {
	};
	usleep(I2C_PAUSE);
	status = XIicPs_MasterSendPolled(&Iic, buffer, 1, HDMI_I2C_ADDR);
	while (XIicPs_BusIsBusy(&Iic)) {
	};
	usleep(I2C_PAUSE);
	buffer[1] = 0xff;
	status = XIicPs_MasterRecvPolled(&Iic, buffer + 1, 1, HDMI_I2C_ADDR);

	//printf("[hdmi] old value of 0x%0x: 0x%0x\n",addr,buffer[1]);
	buffer[1] = value;

	while (XIicPs_BusIsBusy(&Iic)) {
	};
	status = XIicPs_MasterSendPolled(&Iic, buffer, 2, HDMI_I2C_ADDR);

	while (XIicPs_BusIsBusy(&Iic)) {
	};
	usleep(I2C_PAUSE);
	status = XIicPs_MasterSendPolled(&Iic, buffer, 1, HDMI_I2C_ADDR);

	while (XIicPs_BusIsBusy(&Iic)) {
	};
	usleep(I2C_PAUSE);
	buffer[1] = 0xff;
	status = XIicPs_MasterRecvPolled(&Iic, buffer + 1, 1, HDMI_I2C_ADDR);

	if (buffer[1] != value) {
		printf("[hdmi] new value of 0x%x: 0x%x (should be 0x%x)\n", addr,
				buffer[1], value);
	}

	return status;
}

int hdmi_ctrl_read_byte(u8 addr, u8* buffer) {
	buffer[0] = addr;
	buffer[1] = 0xff;
	while (XIicPs_BusIsBusy(&Iic)) {
	};
	int status = XIicPs_MasterSendPolled(&Iic, buffer, 1, HDMI_I2C_ADDR);
	while (XIicPs_BusIsBusy(&Iic)) {
	};
	usleep(I2C_PAUSE);
	status = XIicPs_MasterRecvPolled(&Iic, buffer + 1, 1, HDMI_I2C_ADDR);

	return status;
}

static u8 sii9022_init[] = {
	0x1e, 0x00,// TPI Device Power State Control Data (R/W)
	0x09, 0x00, //
	0x0a, 0x00,

	0x60, 0x04, 0x3c, 0x01,	// TPI Interrupt Enable (R/W)

	0x1a, 0x10,	// TPI System Control (R/W)

	0x00, 0x4c,	// PixelClock/10000 - LSB          u16:6
	0x01, 0x1d,	// PixelClock/10000 - MSB
	0x02, 0x70,	// Frequency in HZ - LSB
	0x03, 0x17,	// Vertical Frequency in HZ - MSB
	0x04, 0x70,	// Total Pixels per line - LSB
	0x05, 0x06,	// Total Pixels per line - MSB
	0x06, 0xEE,	// Total Lines - LSB
	0x07, 0x02,	// Total Lines - MSB
	0x08, 0x70, // pixel repeat rate?
	0x1a, 0x00, // CTRL_DATA - bit 1 causes 2 purple extra columns on DVI monitors (probably HDMI mode)
};

void disable_reset_out() {
	XGpioPs Gpio;
	XGpioPs_Config *ConfigPtr;
	ConfigPtr = XGpioPs_LookupConfig(GPIO_DEVICE_ID);
	XGpioPs_CfgInitialize(&Gpio, ConfigPtr, ConfigPtr->BaseAddr);
	int output_pin = 7;

	XGpioPs_SetDirectionPin(&Gpio, output_pin, 1);
	XGpioPs_SetOutputEnablePin(&Gpio, output_pin, 1);
	XGpioPs_WritePin(&Gpio, output_pin, 0);
	usleep(10000);
	XGpioPs_WritePin(&Gpio, output_pin, 1);

	print("GPIO reset disable done.\n\r");
}

void hdmi_ctrl_init() {
	int status;
	XIicPs_Config *config;
	config = XIicPs_LookupConfig(IIC_DEVICE_ID);
	status = XIicPs_CfgInitialize(&Iic, config, config->BaseAddress);
	//printf("XIicPs_CfgInitialize: %d\n", status);
	usleep(10000);
	//printf("XIicPs is ready: %lx\n", Iic.IsReady);

	status = XIicPs_SelfTest(&Iic);
	//printf("XIicPs_SelfTest: %x\n", status);

	status = XIicPs_SetSClk(&Iic, IIC_SCLK_RATE);
	//printf("XIicPs_SetSClk: %x\n", status);

	usleep(2500);

	// reset
	status = hdmi_ctrl_write_byte(0xc7, 0);

	u8 buffer[2];
	status = hdmi_ctrl_read_byte(0x1b, buffer);
	//printf("[%d] TPI device id: 0x%x\n", status, buffer[1]);
	status = hdmi_ctrl_read_byte(0x1c, buffer);
	//printf("[%d] TPI revision 1: 0x%x\n",status,buffer[1]);
	//status = hdmi_ctrl_read_byte(0x1d,buffer);
	//printf("[%d] TPI revision 2: 0x%x\n",status,buffer[1]);
	//status = hdmi_ctrl_read_byte(0x30,buffer);
	//printf("[%d] HDCP revision: 0x%x\n",status,buffer[1]);
	//status = hdmi_ctrl_read_byte(0x3d,buffer);
	//printf("[%d] hotplug: 0x%x\n", status, buffer[1]);

	for (int i = 0; i < sizeof(sii9022_init); i += 2) {
		status = hdmi_ctrl_write_byte(sii9022_init[i], sii9022_init[i + 1]);
		usleep(1);
	}
}

XAxiVdma vdma;
u32* framebuffer = 0;
u32 bgbuf_offset = 0;

uint16_t split_pos = 0, next_split_pos = 0;
u32 framebuffer_pan_offset = 0;
static u32 blitter_dst_offset = 0;
static u32 blitter_src_offset = 0;
static u32 vmode_hsize = 800, vmode_vsize = 600, vmode_hdiv = 1, vmode_vdiv = 2;

extern u32 fb_pitch;
extern u32 *fb;

//extern uint32_t* fb=0;
//extern uint32_t fb_pitch=0;

// 32bit: hdiv=1, 16bit: hdiv=2, 8bit: hdiv=4, ...
int init_vdma(int hsize, int vsize, int hdiv, int vdiv, u32 bufpos) {
	int status;
	XAxiVdma_Config *Config;

	Config = XAxiVdma_LookupConfig(VDMA_DEVICE_ID);

	if (!Config) {
		printf("VDMA not found for ID %d\r\n", VDMA_DEVICE_ID);
		return XST_FAILURE;
	}

	/*XAxiVdma_DmaStop(&vdma, XAXIVDMA_READ);
	 XAxiVdma_Reset(&vdma, XAXIVDMA_READ);
	 XAxiVdma_ClearDmaChannelErrors(&vdma, XAXIVDMA_READ, XAXIVDMA_SR_ERR_ALL_MASK);*/

	status = XAxiVdma_CfgInitialize(&vdma, Config, Config->BaseAddress);
	if (status != XST_SUCCESS) {
		printf("VDMA Configuration Initialization failed, status: 0x%X\r\n",
				status);
		//return status;
	}

	u32 stride = hsize * (Config->Mm2SStreamWidth >> 3);

	XAxiVdma_DmaSetup ReadCfg;

	//printf("VDMA HDIV: %d VDIV: %d\n", hdiv, vdiv);

	ReadCfg.VertSizeInput = vsize / vdiv;
	ReadCfg.HoriSizeInput = stride / hdiv; // note: changing this breaks the output
	ReadCfg.Stride = stride / hdiv; // note: changing this is not a problem
	ReadCfg.FrameDelay = 0; /* This example does not test frame delay */
	ReadCfg.EnableCircularBuf = 1; /* Only 1 buffer, continuous loop */
	ReadCfg.EnableSync = 0; /* Gen-Lock */
	ReadCfg.PointNum = 0;
	ReadCfg.EnableFrameCounter = 0; /* Endless transfers */
	ReadCfg.FixedFrameStoreAddr = 0; /* We are not doing parking */

	ReadCfg.FrameStoreStartAddr[0] = bufpos;

	//printf("VDMA Framebuffer at 0x%x\n", ReadCfg.FrameStoreStartAddr[0]);

	status = XAxiVdma_DmaConfig(&vdma, XAXIVDMA_READ, &ReadCfg);
	if (status != XST_SUCCESS) {
		printf("VDMA Read channel config failed, status: 0x%X\r\n", status);
		return status;
	}

	status = XAxiVdma_DmaSetBufferAddr(&vdma, XAXIVDMA_READ, ReadCfg.FrameStoreStartAddr);
	if (status != XST_SUCCESS) {
		printf("VDMA Read channel set buffer address failed, status: 0x%X\r\n", status);
		return status;
	}

	status = XAxiVdma_DmaStart(&vdma, XAXIVDMA_READ);
	if (status != XST_SUCCESS) {
		printf("VDMA Failed to start DMA engine (read channel), status: 0x%X\r\n", status);
		return status;
	}
	return XST_SUCCESS;
}

void hdmi_set_video_mode(u16 htotal, u16 vtotal, u32 pixelclock_hz, u16 vhz, u8 hdmi) {
	/*
	 * SII9022 registers
	 *
	 0x00, 0x4c,	// PixelClock/10000 - LSB
	 0x01, 0x1d,	// PixelClock/10000 - MSB
	 0x02, 0x70,	// Frequency in HZ - LSB
	 0x03, 0x17,	// Vertical Frequency in HZ - MSB
	 0x04, 0x70,	// Total Pixels per line - LSB
	 0x05, 0x06,	// Total Pixels per line - MSB
	 0x06, 0xEE,	// Total Lines - LSB
	 0x07, 0x02,	// Total Lines - MSB
	 0x08, 0x70, // pixel repeat rate?
	 0x1a, 0x00, // 0: DVI, 1: HDMI
	 */

	// see also https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/bridge/sii902x.c#L358
	u8* sii_mode = sii9022_init + 12;

	sii_mode[2 * 0 + 1] = pixelclock_hz / 10000;
	sii_mode[2 * 1 + 1] = (pixelclock_hz / 10000) >> 8;
	sii_mode[2 * 2 + 1] = vhz * 100;
	sii_mode[2 * 3 + 1] = (vhz * 100) >> 8;
	sii_mode[2 * 4 + 1] = htotal;
	sii_mode[2 * 5 + 1] = htotal >> 8;
	sii_mode[2 * 6 + 1] = vtotal;
	sii_mode[2 * 7 + 1] = vtotal >> 8;
	sii_mode[2 * 9 + 1] = hdmi;
}

u32 dump_vdma_status(XAxiVdma *InstancePtr) {
	u32 status = XAxiVdma_GetStatus(InstancePtr, XAXIVDMA_READ);

	xil_printf("Read channel dump\n\r");
	xil_printf("\tMM2S DMA Control Register: %x\r\n",
			XAxiVdma_ReadReg(InstancePtr->ReadChannel.ChanBase,
					XAXIVDMA_CR_OFFSET));
	xil_printf("\tMM2S DMA Status Register: %x\r\n",
			XAxiVdma_ReadReg(InstancePtr->ReadChannel.ChanBase,
					XAXIVDMA_SR_OFFSET));
	xil_printf("\tMM2S HI_FRMBUF Reg: %x\r\n",
			XAxiVdma_ReadReg(InstancePtr->ReadChannel.ChanBase,
					XAXIVDMA_HI_FRMBUF_OFFSET));
	xil_printf("\tFRMSTORE Reg: %d\r\n",
			XAxiVdma_ReadReg(InstancePtr->ReadChannel.ChanBase,
					XAXIVDMA_FRMSTORE_OFFSET));
	xil_printf("\tBUFTHRES Reg: %d\r\n",
			XAxiVdma_ReadReg(InstancePtr->ReadChannel.ChanBase,
					XAXIVDMA_BUFTHRES_OFFSET));
	xil_printf("\tMM2S Vertical Size Register: %d\r\n",
			XAxiVdma_ReadReg(InstancePtr->ReadChannel.ChanBase,
					XAXIVDMA_MM2S_ADDR_OFFSET + XAXIVDMA_VSIZE_OFFSET));
	xil_printf("\tMM2S Horizontal Size Register: %d\r\n",
			XAxiVdma_ReadReg(InstancePtr->ReadChannel.ChanBase,
					XAXIVDMA_MM2S_ADDR_OFFSET + XAXIVDMA_HSIZE_OFFSET));
	xil_printf("\tMM2S Frame Delay and Stride Register: %d\r\n",
			XAxiVdma_ReadReg(InstancePtr->ReadChannel.ChanBase,
					XAXIVDMA_MM2S_ADDR_OFFSET + XAXIVDMA_STRD_FRMDLY_OFFSET));
	xil_printf("\tMM2S Start Address 1: %x\r\n",
			XAxiVdma_ReadReg(InstancePtr->ReadChannel.ChanBase,
					XAXIVDMA_MM2S_ADDR_OFFSET + XAXIVDMA_START_ADDR_OFFSET));

	xil_printf("VDMA status: ");
	if (status & XAXIVDMA_SR_HALTED_MASK)
		xil_printf("halted\n");
	else
		xil_printf("running\n");
	if (status & XAXIVDMA_SR_IDLE_MASK)
		xil_printf("idle\n");
	if (status & XAXIVDMA_SR_ERR_INTERNAL_MASK)
		xil_printf("internal err\n");
	if (status & XAXIVDMA_SR_ERR_SLAVE_MASK)
		xil_printf("slave err\n");
	if (status & XAXIVDMA_SR_ERR_DECODE_MASK)
		xil_printf("decode err\n");
	if (status & XAXIVDMA_SR_ERR_FSZ_LESS_MASK)
		xil_printf("FSize Less Mismatch err\n");
	if (status & XAXIVDMA_SR_ERR_LSZ_LESS_MASK)
		xil_printf("LSize Less Mismatch err\n");
	if (status & XAXIVDMA_SR_ERR_SG_SLV_MASK)
		xil_printf("SG slave err\n");
	if (status & XAXIVDMA_SR_ERR_SG_DEC_MASK)
		xil_printf("SG decode err\n");
	if (status & XAXIVDMA_SR_ERR_FSZ_MORE_MASK)
		xil_printf("FSize More Mismatch err\n");

	return status;
}

void fb_fill(uint32_t offset) {
	memset(framebuffer + offset, 0, 1280 * 1024 * 4);
}

static XClk_Wiz clkwiz;

void pixelclock_init_2(struct zz_video_mode *mode) {
	XClk_Wiz_Config conf;
	XClk_Wiz_CfgInitialize(&clkwiz, &conf, XPAR_CLK_WIZ_0_BASEADDR);

	/*u32 phase = XClk_Wiz_ReadReg(XPAR_CLK_WIZ_0_BASEADDR, 0x20C);
	u32 duty = XClk_Wiz_ReadReg(XPAR_CLK_WIZ_0_BASEADDR, 0x210);
	u32 divide = XClk_Wiz_ReadReg(XPAR_CLK_WIZ_0_BASEADDR, 0x208);
	u32 muldiv = XClk_Wiz_ReadReg(XPAR_CLK_WIZ_0_BASEADDR, 0x200);*/

	u32 mul = mode->mul;
	u32 div = mode->div;
	u32 otherdiv = mode->div2;

	XClk_Wiz_WriteReg(XPAR_CLK_WIZ_0_BASEADDR, 0x200, (mul << 8) | div);
	XClk_Wiz_WriteReg(XPAR_CLK_WIZ_0_BASEADDR, 0x208, otherdiv);

	// load configuration
	XClk_Wiz_WriteReg(XPAR_CLK_WIZ_0_BASEADDR, 0x25C, 0x00000003);
	//XClk_Wiz_WriteReg(XPAR_CLK_WIZ_0_BASEADDR,  0x25C, 0x00000001);

	/*phase = XClk_Wiz_ReadReg(XPAR_CLK_WIZ_0_BASEADDR, 0x20C);
	//printf("CLK phase: %lu\n", phase);
	duty = XClk_Wiz_ReadReg(XPAR_CLK_WIZ_0_BASEADDR, 0x210);
	//printf("CLK duty: %lu\n", duty);
	divide = XClk_Wiz_ReadReg(XPAR_CLK_WIZ_0_BASEADDR, 0x208);
	//printf("CLK divide: %lu\n", divide);
	muldiv = XClk_Wiz_ReadReg(XPAR_CLK_WIZ_0_BASEADDR, 0x200);
	//printf("CLK muldiv: %lu\n", muldiv);*/
}

// FIXME!
#define MNTZ_BASE_ADDR 0x43C00000

#define MNTZORRO_REG0 0
#define MNTZORRO_REG1 4
#define MNTZORRO_REG2 8
#define MNTZORRO_REG3 12
#define MNTZORRO_REG4 16
#define MNTZORRO_REG5 20

#define mntzorro_read(BaseAddress, RegOffset) \
    Xil_In32((BaseAddress) + (RegOffset))

#define mntzorro_write(BaseAddress, RegOffset, Data) \
  	Xil_Out32((BaseAddress) + (RegOffset), (u32)(Data))

void video_formatter_valign() {
	// vertical alignment
	mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG3, 1);
	usleep(1);
	mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG2, 0x80000000 + 0x5); // OP_VSYNC
	usleep(1);
	mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG3, 0);
	usleep(1);
	mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG2, 0x80000000); // NOP
	usleep(1);
	mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG2, 0); // NOP
	usleep(1);
}

#define VF_DLY ;

void video_formatter_write(uint32_t data, uint16_t op) {
	mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG3, data);
	VF_DLY;
	mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG2, 0x80000000 | op); // OP_MAX (vmax | hmax)
	VF_DLY;
	mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG2, 0x80000000); // NOP
	VF_DLY;
	mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG2, 0); // clear
	VF_DLY;
	mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG3, 0); // clear
	VF_DLY;
}

void video_formatter_init(int scalemode, int colormode, int width, int height,
		int htotal, int vtotal, int hss, int hse, int vss, int vse,
		int polarity) {
	video_formatter_write((vtotal << 16) | htotal, MNTVF_OP_MAX);
	video_formatter_write((height << 16) | width, MNTVF_OP_DIMENSIONS);
	video_formatter_write((hss << 16) | hse, MNTVF_OP_HS);
	video_formatter_write((vss << 16) | vse, MNTVF_OP_VS);
	video_formatter_write(polarity, MNTVF_OP_POLARITY);
	video_formatter_write(scalemode, MNTVF_OP_SCALE);
	video_formatter_write(colormode, MNTVF_OP_COLORMODE);

	video_formatter_valign();
}

void video_system_init(struct zz_video_mode *mode, int hdiv, int vdiv) {
	pixelclock_init_2(mode);
	hdmi_ctrl_init();
	init_vdma(mode->hres, mode->vres, hdiv, vdiv, (u32)framebuffer + framebuffer_pan_offset);
}

// Our address space is relative to the autoconfig base address (for example, it could be 0x600000)
#define MNT_REG_BASE    			0x000000
// Frame buffer/graphics memory starts at 64KB, leaving ample space for general purpose registers.
#define MNT_FB_BASE     			0x010000

#define REVISION_MAJOR 1
#define REVISION_MINOR 8

int scalemode = 0;

void video_mode_init(int mode, int scalemode, int colormode) {
	int hdiv = 1, vdiv = 1;

	if (scalemode & 1)
		hdiv = 2;
	if (scalemode & 2)
		vdiv = 2;

	if (colormode == 0)
		hdiv *= 4;
	if (colormode == 1)
		hdiv *= 2;

	struct zz_video_mode *vmode = &preset_video_modes[mode];

	video_system_init(vmode, hdiv, vdiv);

	video_formatter_init(scalemode, colormode,
			vmode->hres, vmode->vres,
			vmode->hmax, vmode->vmax,
			vmode->hstart, vmode->hend,
			vmode->vstart, vmode->vend,
			vmode->polarity);

	vmode_hsize = vmode->hres;
	vmode_vsize = vmode->vres;
	vmode_vdiv = vdiv;
	vmode_hdiv = hdiv;
}

int16_t sprite_x = 0, sprite_x_adj = 0, sprite_x_base = 0;
int16_t sprite_y = 0, sprite_y_adj = 0, sprite_y_base = 0;
int16_t sprite_x_offset = 0;
int16_t sprite_y_offset = 0;

uint16_t sprite_enabled = 0;
uint8_t sprite_width  = 16;
uint8_t sprite_height = 16;

uint32_t sprite_buf[32 * 48];
uint8_t sprite_clipped = 0;
int16_t sprite_clip_x = 0, sprite_clip_y = 0;

uint32_t sprite_colors[4] = { 0x00ff00ff, 0x00000000, 0x00000000, 0x00000000 };

uint8_t sprite_template[16*16] = {
		0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,
		0,0,0,3,3,1,1,0,0,0,0,0,0,0,0,0,
		0,0,0,2,3,3,3,1,1,0,0,0,0,0,0,0,
		0,0,0,2,3,3,3,3,3,1,1,0,0,0,0,0,
		0,0,0,0,2,3,3,3,3,3,3,1,1,0,0,0,
		0,0,0,0,2,3,3,3,3,3,3,3,3,1,1,0,
		0,0,0,0,0,2,3,3,3,3,3,3,3,3,2,0,
		0,0,0,0,0,2,3,3,3,3,3,3,3,2,2,0,
		0,0,0,0,0,0,2,3,3,3,3,3,3,2,0,0,
		0,0,0,0,0,0,2,3,3,3,3,3,3,1,0,0,
		0,0,0,0,0,0,0,2,3,3,2,2,3,3,1,0,
		0,0,0,0,0,0,0,2,3,2,2,2,2,3,3,1,
		0,0,0,0,0,0,0,0,2,2,0,0,2,2,3,2,
		0,0,0,0,0,0,0,0,2,0,0,0,0,2,2,2,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,0,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

void sprite_hide() {
	sprite_x = 2000;
	sprite_y = 2000;
	sprite_enabled = 0;
	video_formatter_write((sprite_y << 16) | sprite_x, MNTVF_OP_SPRITE_XY);
}

void sprite_reset() {
	sprite_hide();

	for (int y=0; y<16; y++) {
		for (int x=0; x<16; x++) {
			uint8_t addr = y*16+x;
			uint32_t data = 0xff00ff;
			if (sprite_template[y*16+x]==1) {
				data = 0xffffff;
			} else if (sprite_template[y*16+x]==2) {
				data = 0x000000;
			} else if (sprite_template[y*16+x]==3) {
				data = (255-15*y)<<16;
			}
			video_formatter_write((addr << 24) | data, MNTVF_OP_SPRITE_DATA);
		}
	}
}

void update_hw_sprite_pos(int16_t x, int16_t y) {
	sprite_x = x + sprite_x_offset + 1;
	// horizontally doubled mode
	if (scalemode & 1)
		sprite_x_adj = (sprite_x * 2) + 1;
	else
		sprite_x_adj = sprite_x + 2;

	sprite_y = y + split_pos + sprite_y_offset + 1;

	// vertically doubled mode
	if (scalemode & 2)
		sprite_y_adj = sprite_y *= 2;
	else
		sprite_y_adj = sprite_y;

	if (sprite_x < 0 || sprite_y < 0) {
		if (sprite_clip_x != sprite_x || sprite_clip_y != sprite_y) {
			clip_hw_sprite((sprite_x < 0) ? sprite_x : 0, (sprite_y < 0) ? sprite_y : 0);
		}
		sprite_clipped = 1;
		if (sprite_x < 0) {
			sprite_x_adj = 0;
			sprite_clip_x = sprite_x;
		}
		if (sprite_y < 0) {
			sprite_y_adj = 0;
			sprite_clip_y = sprite_y;
		}
	}
	else if (sprite_clipped && sprite_x >= 0 && sprite_y >= 0) {
		clip_hw_sprite(0, 0);
		sprite_clipped = 0;
	}
	video_formatter_write((sprite_y_adj << 16) | sprite_x_adj, MNTVF_OP_SPRITE_XY);
}

// this mode can be changed by amiga software to select a different resolution / framerate for
// native video capture
//static int videocap_video_mode = ZZVMODE_720x576;
//static int video_mode = ZZVMODE_720x576|2<<12|MNTVA_COLOR_32BIT<<8;
//static int default_pan_offset = 0x00e00000;

// default to more compatible 60hz mode
static int videocap_video_mode = ZZVMODE_800x600;
static int video_mode = ZZVMODE_800x600 | 2 << 12 | MNTVA_COLOR_32BIT << 8;
static int default_pan_offset = 0x00e00bf8;
static char usb_storage_available = 0;
static uint32_t usb_storage_read_block = 0;
static uint32_t usb_storage_write_block = 0;

// ethernet state
uint16_t ethernet_send_result = 0;

// usb state
uint16_t usb_status = 0;
// we can read or write a number of USB blocks at once, and amiga can select which one is mapped at the USB storage buffer area
uint32_t usb_selected_buffer_block = 0;
uint32_t usb_read_write_num_blocks = 1;
// debug things like individual reads/writes, greatly slowing the system down
uint32_t debug_lowlevel = 0;

void videocap_area_clear() {
	fb_fill(0x00e00000 / 4);
}

void reset_default_videocap_pan() {
	if (videocap_video_mode == ZZVMODE_800x600) {
		default_pan_offset = 0x00e00bf8;
	} else {
		default_pan_offset = 0x00e00000;
	}
}

#define INTC_INTERRUPT_ID_0 61 // IRQ_F2P[0:0]
#define INTC_INTERRUPT_ID_1 62 // IRQ_F2P[1:1]
static XScuGic intc;

static int isr_flush_count=0;

// interrupt service routine for IRQ_F2P[0:0]
// vblank + raster position interrupt
void isr0 (void *intc_inst_ptr) {
	u32 zstate = mntzorro_read(MNTZ_BASE_ADDR, MNTZORRO_REG3);

	int videocap_enabled = (zstate & (1 << 23));
	int vblank = (zstate & (1 << 21));

	// videocap vdma handling is still in main loop
	if (!videocap_enabled) {
		if (!vblank) {
			// if this is not the vblank interrupt, set up the split buffer
			// TODO: VDMA doesn't seem to like switching buffers in the middle of a frame.
			// the first line after a switch contains an extraneous word, so we end up
			// with up to 4 pixels of the other buffer in the first line
			if (split_pos != 0) {
				init_vdma(vmode_hsize, vmode_vsize, vmode_hdiv, vmode_vdiv, (u32)framebuffer + bgbuf_offset);
			}
		} else {
			// if this is the vblank interrupt, set up the "normal" buffer
			init_vdma(vmode_hsize, vmode_vsize, vmode_hdiv, vmode_vdiv, (u32)framebuffer + framebuffer_pan_offset);

			split_pos = next_split_pos;
		}
	}

	// flush the data caches synchronized to full frames
	if (!vblank || (split_pos == 0)) {
		//if (isr_flush_count > 0) {
			Xil_L1DCacheFlush();
			Xil_L2CacheFlush();
			isr_flush_count = 0;
		//} else {
		//	isr_flush_count++;
		//}
	}
}

int fpga_interrupt_init() {
  int result;
  XScuGic *intc_instance_ptr = &intc;
  XScuGic_Config *intc_config;

  // get config for interrupt controller
  intc_config = XScuGic_LookupConfig(XPAR_PS7_SCUGIC_0_DEVICE_ID);
  if (NULL == intc_config) {
    return XST_FAILURE;
  }

  printf("XScuGic_CfgInitialize()\n");

  // initialize the interrupt controller driver
  result = XScuGic_CfgInitialize(intc_instance_ptr, intc_config, intc_config->CpuBaseAddress);

  if (result != XST_SUCCESS) {
    return result;
  }

  printf("XScuGic_SetPriorityTriggerType()\n");

  // set the priority of IRQ_F2P[0:0] to 0xA0 (highest 0xF8, lowest 0x00) and a trigger for a rising edge 0x3.
  XScuGic_SetPriorityTriggerType(intc_instance_ptr, INTC_INTERRUPT_ID_0, 0xA0, 0x3);

  printf("XScuGic_Connect()\n");

  // connect the interrupt service routine isr0 to the interrupt controller
  result = XScuGic_Connect(intc_instance_ptr, INTC_INTERRUPT_ID_0, (Xil_ExceptionHandler)isr0, (void *)&intc);

  if (result != XST_SUCCESS) {
    return result;
  }

  printf("XScuGic_Enable()\n");

  // enable interrupts for IRQ_F2P[0:0]
  XScuGic_Enable(intc_instance_ptr, INTC_INTERRUPT_ID_0);

  return 0;
}

void handle_amiga_reset() {
	reset_default_videocap_pan();

	framebuffer_pan_offset = default_pan_offset;
	videocap_area_clear();

	printf("    _______________   ___   ___   ___  \n");
	printf("   |___  /___  / _ \\ / _ \\ / _ \\ / _ \\ \n");
	printf("      / /   / / (_) | | | | | | | | | |\n");
	printf("     / /   / / \\__, | | | | | | | | | |\n");
	printf("    / /__ / /__  / /| |_| | |_| | |_| |\n");
	printf("   /_____/_____|/_/  \\___/ \\___/ \\___/ \n\n");

	usleep(10000);

	// scalemode 2 (vertical doubling)
	video_mode_init(videocap_video_mode, 2, MNTVA_COLOR_32BIT);
	video_mode = videocap_video_mode | 2 << 12 | MNTVA_COLOR_32BIT << 8;

	sprite_reset();
	ethernet_init();
	fpga_interrupt_init();

	usb_storage_available = zz_usb_init();

	usb_status = 0;
	usb_selected_buffer_block = 0;
	usb_read_write_num_blocks = 1;
	ethernet_send_result = 0;

	cur_mem_offset = 0x3500000;

	// FIXME there should be more state to be reset
}

uint16_t arm_app_output_event_serial = 0;
uint16_t arm_app_output_event_code = 0;
char arm_app_output_event_ack = 0;
uint16_t arm_app_output_events_blocking = 0;
uint16_t arm_app_output_putchar_to_events = 0;
uint16_t arm_app_input_event_serial = 0;
uint16_t arm_app_input_event_code = 0;
char arm_app_input_event_ack = 0;

uint32_t arm_app_output_events_timeout = 100000;

void arm_app_put_event_code(uint16_t code) {
	arm_app_output_event_code = code;
	arm_app_output_event_ack = 0;
	arm_app_output_event_serial++;
}

char arm_app_output_event_acked() {
	return arm_app_output_event_ack;
}

void arm_app_set_output_events_blocking(char blocking) {
	arm_app_output_events_blocking = blocking;
}

void arm_app_set_output_putchar_to_events(char putchar_enabled) {
	arm_app_output_putchar_to_events = putchar_enabled;
}

uint16_t arm_app_get_event_serial() {
	return arm_app_input_event_serial;
}

uint16_t arm_app_get_event_code() {
	arm_app_input_event_ack = 1;
	return arm_app_input_event_code;
}

int __attribute__ ((visibility ("default"))) _putchar(char c) {
	if (arm_app_output_putchar_to_events) {
		if (arm_app_output_events_blocking) {
			for (uint32_t i = 0; i < arm_app_output_events_timeout; i++) {
				usleep(1);
				if (arm_app_output_event_ack)
					break;
			}
		}
		arm_app_put_event_code(c);
	}
	return putchar(c);
}

struct ZZ9K_ENV {
	uint32_t api_version;
	uint32_t argv[8];
	uint32_t argc;

	int (*fn_putchar)(char);
	void (*fn_set_output_putchar_to_events)(char);
	void (*fn_set_output_events_blocking)(char);
	void (*fn_put_event_code)(uint16_t);
	uint16_t (*fn_get_event_serial)();
	uint16_t (*fn_get_event_code)();
	char (*fn_output_event_acked)();
};

void arm_exception_handler_id_reset(void *callback);
void arm_exception_handler_id_data_abort(void *callback);
void arm_exception_handler_id_prefetch_abort(void *callback);
void arm_exception_handler_illinst(void *callback);
void DataAbort_InterruptHandler(void *InstancePtr);

volatile struct ZZ9K_ENV arm_run_env;
volatile void (*core1_trampoline)(volatile struct ZZ9K_ENV* env);
volatile int core2_execute = 0;

#pragma GCC push_options
#pragma GCC optimize ("O1")
// core1_loop is executed on core1 (vs core0)
void core1_loop() {
	asm("mov	r0, r0");
	asm("mrc	p15, 0, r1, c1, c0, 2");
	/* read cp access control register (CACR) into r1 */
	asm("orr	r1, r1, #(0xf << 20)");
	/* enable full access for p10 & p11 */
	asm("mcr	p15, 0, r1, c1, c0, 2");
	/* write back into CACR */

	// enable FPU
	asm("fmrx	r1, FPEXC");
	/* read the exception register */
	asm("orr	r1,r1, #0x40000000");
	/* set VFP enable bit, leave the others in orig state */
	asm("fmxr	FPEXC, r1");
	/* write back the exception register */

	// enable flow prediction
	asm("mrc	p15,0,r0,c1,c0,0");
	/* flow prediction enable */
	asm("orr	r0, r0, #(0x01 << 11)");
	/* #0x8000 */
	asm("mcr	p15,0,r0,c1,c0,0");

	asm("mrc	p15,0,r0,c1,c0,1");
	/* read Auxiliary Control Register */
	asm("orr	r0, r0, #(0x1 << 2)");
	/* enable Dside prefetch */
	asm("orr	r0, r0, #(0x1 << 1)");
	/* enable L2 Prefetch hint */
	asm("mcr	p15,0,r0,c1,c0,1");
	/* write Auxiliary Control Register */

	// stack
	asm("mov sp, #0x06000000");

	volatile uint32_t* addr = 0;
	addr[0] = 0xe3e0000f; // mvn	r0, #15  -- loads 0xfffffff0
	addr[1] = 0xe590f000; // ldr	pc, [r0] -- jumps to the address in that address

	// FIXME these don't seem to do anything useful yet
	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_RESET,
			(Xil_ExceptionHandler) arm_exception_handler_id_reset, NULL);
	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_DATA_ABORT_INT,
			(Xil_ExceptionHandler) arm_exception_handler_id_data_abort, NULL);
	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_PREFETCH_ABORT_INT,
			(Xil_ExceptionHandler) arm_exception_handler_id_prefetch_abort, NULL);
	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_UNDEFINED_INT,
			(Xil_ExceptionHandler) arm_exception_handler_illinst, NULL);

	while (1) {
		while (!core2_execute) {
			usleep(1);
		}
		core2_execute = 0;
		printf("[CPU1] executing at %p.\n", core1_trampoline);
		Xil_DCacheFlush();
		Xil_ICacheInvalidate();

		asm("push {r0-r12}");
		// FIXME HACK save our stack pointer in 0x10000
		asm("mov r0, #0x00010000");
		asm("str sp, [r0]");

		core1_trampoline(&arm_run_env);

		asm("mov r0, #0x00010000");
		asm("ldr sp, [r0]");
		asm("pop {r0-r12}");
	}
}
#pragma GCC pop_options

int main() {
	const char* zstates[53] = { "RESET   ", "Z2_CONF ", "Z2_IDLE ", "WAIT_WRI",
			"WAIT_WR2", "Z2WRIFIN", "WAIT_RD ", "WAIT_RD2", "WAIT_RD3",
			"CONFIGED", "CONF_CLR", "D_Z2_Z3 ", "Z3_IDLE ", "Z3_WRITE_UPP",
			"Z3_WRITE_LOW", "Z3_READ_UP", "Z3_READ_LOW", "Z3_READ_DLY",
			"Z3_READ_DLY1", "Z3_READ_DLY2", "Z3_WRITE_PRE", "Z3_WRITE_FIN",
			"Z3_ENDCYCLE", "Z3_DTACK", "Z3_CONFIG", "Z2_REGWRITE", "REGWRITE",
			"REGREAD", "Z2_REGR_POST", "Z3_REGR_POST", "Z3_REGWRITE",
			"Z2_REGREAD", "Z3_REGREAD", "NONE_33", "Z2_PRE_CONF", "Z2_ENDCYCLE",
			"NONE_36", "NONE_37", "NONE_38", "RESET_DVID", "COLD", "WR2B",
			"WR2C", "Z3DMA1", "Z3DMA2", "Z3_AUTOCONF_RD", "Z3_AUTOCONF_WR",
			"Z3_AUTOCONF_RD_DLY", "Z3_AUTOCONF_RD_DLY2", "Z3_REGWRITE_PRE",
			"Z3_REGREAD_PRE", "Z3_WRITE_PRE2", "UNDEF", };

	init_platform();

	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_DATA_ABORT_INT,
			(Xil_ExceptionHandler) arm_exception_handler_id_data_abort, NULL);
	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_PREFETCH_ABORT_INT,
			(Xil_ExceptionHandler) arm_exception_handler_id_prefetch_abort, NULL);
	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_UNDEFINED_INT,
			(Xil_ExceptionHandler) arm_exception_handler_illinst, NULL);

	memset((u32 *)Z3_SCRATCH_ADDR, 0, sizeof(struct GFXData));

	disable_reset_out();

	xadc_init();

	// FIXME constant
	framebuffer = (u32*) 0x00200000;
	int need_req_ack = 0;
	u8* mem = (u8*) framebuffer;

	// blitter etc
	uint16_t rect_x1 = 0;
	uint16_t rect_x2 = 0;
	uint16_t rect_x3 = 0;
	uint16_t rect_y1 = 0;
	uint16_t rect_y2 = 0;
	uint16_t rect_y3 = 0;
	uint16_t blitter_dst_pitch = 0;
	uint32_t rect_rgb = 0;
	uint32_t rect_rgb2 = 0;
	uint32_t blitter_colormode = MNTVA_COLOR_32BIT;
	uint16_t blitter_src_pitch = 0;

	uint16_t blitter_user1 = 0;
	uint16_t blitter_user2 = 0;
	uint16_t blitter_user3 = 0;
	uint16_t blitter_user4 = 0;

	// ARM app run environment
	arm_run_env.api_version = 1;
	arm_run_env.fn_putchar = _putchar;
	arm_run_env.fn_get_event_code = arm_app_get_event_code;
	arm_run_env.fn_get_event_serial = arm_app_get_event_serial;
	arm_run_env.fn_output_event_acked = arm_app_output_event_acked;
	arm_run_env.fn_put_event_code = arm_app_put_event_code;
	arm_run_env.fn_set_output_events_blocking =
			arm_app_set_output_events_blocking;
	arm_run_env.fn_set_output_putchar_to_events =
			arm_app_set_output_putchar_to_events;

	arm_run_env.argc = 0;
	uint32_t arm_run_address = 0;

	// zorro state
	u32 zstate_raw;
	int interlace_old = 0;
	int videocap_ntsc_old = 0;

	handle_amiga_reset();

	printf("launch core1...\n");
	volatile uint32_t* core1_addr = (volatile uint32_t*) 0xFFFFFFF0;
	*core1_addr = (uint32_t) core1_loop;
	// Place some machine code in strategic positions that will catch core1 if it crashes
	// FIXME: clean this up and turn into a debug handler / monitor
	volatile uint32_t* core1_addr2 = (volatile uint32_t*) 0x140; // catch 1
	core1_addr2[0] = 0xe3e0000f; // mvn	r0, #15  -- loads 0xfffffff0
	core1_addr2[1] = 0xe590f000; // ldr	pc, [r0] -- jumps to the address in that address

	core1_addr2 = (volatile uint32_t*) 0x100; // catch 2
	core1_addr2[0] = 0xe3e0000f; // mvn	r0, #15  -- loads 0xfffffff0
	core1_addr2[1] = 0xe590f000; // ldr	pc, [r0] -- jumps to the address in that address

	asm("sev");
	printf("core1 now idling.\n");

	int videocap_enabled_old = 1;
	int colormode = 0;
	video_mode = 0x2200;

	int backlog_nag_counter = 0;
	int interrupt_enabled = 0;

	int custom_video_mode = ZZVMODE_CUSTOM;
	int custom_vmode_param = VMODE_PARAM_HRES;

	while (1) {
		u32 zstate = mntzorro_read(MNTZ_BASE_ADDR, MNTZORRO_REG3);
		zstate_raw = zstate;
		u32 writereq = (zstate & (1 << 31));
		u32 readreq = (zstate & (1 << 30));

		zstate = zstate & 0xff;
		if (zstate > 52) zstate = 52;

		if (writereq) {
			u32 zaddr = mntzorro_read(MNTZ_BASE_ADDR, MNTZORRO_REG0);
			u32 zdata = mntzorro_read(MNTZ_BASE_ADDR, MNTZORRO_REG1);

			u32 ds3 = (zstate_raw & (1 << 29));
			u32 ds2 = (zstate_raw & (1 << 28));
			u32 ds1 = (zstate_raw & (1 << 27));
			u32 ds0 = (zstate_raw & (1 << 26));

			if (debug_lowlevel) {
				printf("WRTE: %08lx <- %08lx [%d%d%d%d]\n",zaddr,zdata,!!ds3,!!ds2,!!ds1,!!ds0);
			}

			if (zaddr > 0x10000000) {
				printf("ERRW illegal address %08lx\n", zaddr);
			} else if (zaddr >= MNT_FB_BASE || zaddr >= MNT_REG_BASE + 0x2000) {
				u8* ptr = mem;

				if (zaddr >= MNT_FB_BASE) {
					ptr = mem + zaddr - MNT_FB_BASE;
				} else if (zaddr < MNT_REG_BASE + 0x8000) {
					// FIXME remove
					ptr = (u8*) (RX_FRAME_ADDRESS + zaddr - (MNT_REG_BASE + 0x2000));
					//printf("ERXF write: %08lx\n", (u32) ptr);
				} else if (zaddr < MNT_REG_BASE + 0xa000) {
					ptr = (u8*) (TX_FRAME_ADDRESS + zaddr - (MNT_REG_BASE + 0x8000));
				} else if (zaddr < MNT_REG_BASE + 0x10000) {
					// 0xa000-0xafff: write to block device (usb storage)
					// TODO: this should be moved to DMA space?
					ptr = (u8*) (USB_BLOCK_STORAGE_ADDRESS + zaddr - (MNT_REG_BASE + 0xa000) + usb_selected_buffer_block * 512);
				}

				// FIXME cache this
				u32 z3 = (zstate_raw & (1 << 25));

				if (z3) {
					if (ds3) ptr[0] = zdata >> 24;
					if (ds2) ptr[1] = zdata >> 16;
					if (ds1) ptr[2] = zdata >> 8;
					if (ds0) ptr[3] = zdata;
				} else {
					// swap bytes
					if (ds1) ptr[0] = zdata >> 8;
					if (ds0) ptr[1] = zdata;
				}
			} else if (zaddr >= MNT_REG_BASE && zaddr < MNT_FB_BASE) {
				// register area
				//printf("REGW: %08lx <- %08lx [%d%d%d%d]\n",zaddr,zdata,!!ds3,!!ds2,!!ds1,!!ds0);

				u32 z3 = (zstate_raw & (1 << 25));
				if (z3) {
					// convert 32bit to 16bit addresses
					if (ds3 && ds2) {
						zdata = zdata >> 16;
					} else if (ds1 && ds0) {
						zdata = zdata & 0xffff;
						zaddr += 2;
					} else {
						zaddr = 0; // cancel
					}
				}
				//printf("CONV: %08lx <- %08lx\n",zaddr,zdata);

				switch (zaddr) {
				// Various blitter/video registers
				case REG_ZZ_PAN_HI:
					framebuffer_pan_offset = zdata << 16;
					break;
				case REG_ZZ_PAN_LO:
					framebuffer_pan_offset |= zdata;

					// cursor offset support for p96 split screen
					sprite_x_offset = rect_x1;
					sprite_y_offset = rect_y1;

					break;

				case REG_ZZ_BLIT_SRC_HI:
					blitter_src_offset = zdata << 16;
					break;
				case REG_ZZ_BLIT_SRC_LO:
					blitter_src_offset |= zdata;
					break;
				case REG_ZZ_BLIT_DST_HI:
					blitter_dst_offset = zdata << 16;
					break;
				case REG_ZZ_BLIT_DST_LO:
					blitter_dst_offset |= zdata;
					break;

				case REG_ZZ_COLORMODE:
					blitter_colormode = zdata;
					break;
				case REG_ZZ_CONFIG:
					// enable/disable INT6, currently used to signal incoming ethernet packets
					interrupt_enabled = zdata & 1;
					break;
				case REG_ZZ_MODE: {
					//printf("mode change: %lx\n", zdata);

					int mode = zdata & 0xff;
					colormode = (zdata & 0xf00) >> 8;
					scalemode = (zdata & 0xf000) >> 12;
					/*printf("mode: %d color: %d scale: %d\n", mode,
							colormode, scalemode);*/

					video_mode_init(mode, scalemode, colormode);
					// remember selected video mode
					video_mode = zdata;
					//request_video_align = 1;
					break;
				}
				case REG_ZZ_VCAP_MODE:
					//printf("videocap default mode select: %lx\n", zdata);

					videocap_video_mode = zdata &0xff;
					break;
				//case REG_ZZ_SPRITE_X:
				case REG_ZZ_SPRITE_Y:
					if (!sprite_enabled)
						break;

					sprite_x_base = (int16_t)rect_x1;
					sprite_y_base = (int16_t)rect_y1;

					update_hw_sprite_pos(sprite_x_base, sprite_y_base);

					break;
				case REG_ZZ_SPRITE_BITMAP: {
					if (zdata == 1) { // Hardware sprite enabled
						sprite_enabled = 1;
						break;
					}
					else if (zdata == 2) { // Hardware sprite disabled
						sprite_hide();
						break;
					}

					uint8_t* bmp_data = (uint8_t*) ((u32) framebuffer
							+ blitter_src_offset);

					clear_hw_sprite();

					sprite_x_offset = rect_x1;
					sprite_y_offset = rect_y1;
					sprite_width  = rect_x2;
					sprite_height = rect_y2;

					update_hw_sprite(bmp_data, sprite_colors, sprite_width, sprite_height);
					update_hw_sprite_pos(sprite_x_base, sprite_y_base);
					break;
				}
				case REG_ZZ_SPRITE_COLORS: {
					sprite_colors[zdata] = (blitter_user1 << 16) | blitter_user2;
					if (zdata != 0 && sprite_colors[zdata] == 0xff00ff)
                		sprite_colors[zdata] = 0xfe00fe;
					break;
				}
				case REG_ZZ_SRC_PITCH:
					blitter_src_pitch = zdata;
					break;

				case REG_ZZ_X1:
					rect_x1 = zdata;
					break;
				case REG_ZZ_Y1:
					rect_y1 = zdata;
					break;
				case REG_ZZ_X2:
					rect_x2 = zdata;
					break;
				case REG_ZZ_Y2:
					rect_y2 = zdata;
					break;
				case REG_ZZ_ROW_PITCH:
					blitter_dst_pitch = zdata;
					break;
				case REG_ZZ_X3:
					rect_x3 = zdata;
					break;
				case REG_ZZ_Y3:
					rect_y3 = zdata;
					break;

				case REG_ZZ_USER1:
					blitter_user1 = zdata;
					break;
				case REG_ZZ_USER2:
					blitter_user2 = zdata;
					break;
				case REG_ZZ_USER3:
					blitter_user3 = zdata;
					break;
				case REG_ZZ_USER4:
					blitter_user4 = zdata;
					break;

				case REG_ZZ_RGB_HI:
					rect_rgb &= 0xffff0000;
					rect_rgb |= (((zdata & 0xff) << 8) | zdata >> 8);
					break;
				case REG_ZZ_RGB_LO:
					rect_rgb &= 0x0000ffff;
					rect_rgb |= (((zdata & 0xff) << 8) | zdata >> 8) << 16;
					break;
				case REG_ZZ_RGB2_HI:
					rect_rgb2 &= 0xffff0000;
					rect_rgb2 |= (((zdata & 0xff) << 8) | zdata >> 8);
					break;
				case REG_ZZ_RGB2_LO:
					rect_rgb2 &= 0x0000ffff;
					rect_rgb2 |= (((zdata & 0xff) << 8) | zdata >> 8) << 16;
					break;

				// Generic acceleration ops
				case REG_ZZ_ACC_OP: {
					handle_acc_op(zdata);
					break;
				}

				// DMA RTG rendering
				case REG_ZZ_BITTER_DMA_OP: {
					handle_blitter_dma_op(zdata);
					break;
				}

				// RTG rendering
				case REG_ZZ_FILLRECT:
					set_fb((uint32_t*) ((u32) framebuffer + blitter_dst_offset),
							blitter_dst_pitch);
					uint8_t mask = zdata;

					if (mask == 0xFF)
						fill_rect_solid(rect_x1, rect_y1, rect_x2, rect_y2,
								rect_rgb, blitter_colormode);
					else
						fill_rect(rect_x1, rect_y1, rect_x2, rect_y2, rect_rgb,
								blitter_colormode, mask);
					break;

				case REG_ZZ_COPYRECT: {
					set_fb((uint32_t*) ((u32) framebuffer + blitter_dst_offset),
							blitter_dst_pitch);
					mask = (blitter_colormode >> 8);

					switch (zdata) {
					case 1: // Regular BlitRect
						if (mask == 0xFF || (mask != 0xFF && (blitter_colormode & 0x0F)) != MNTVA_COLOR_8BIT)
							copy_rect_nomask(rect_x1, rect_y1, rect_x2, rect_y2, rect_x3,
											rect_y3, blitter_colormode & 0x0F,
											(uint32_t*) ((u32) framebuffer
													+ blitter_dst_offset),
											blitter_dst_pitch, MINTERM_SRC);
						else
							copy_rect(rect_x1, rect_y1, rect_x2, rect_y2, rect_x3,
									rect_y3, blitter_colormode & 0x0F,
									(uint32_t*) ((u32) framebuffer
											+ blitter_dst_offset),
									blitter_dst_pitch, mask);
						break;
					case 2: // BlitRectNoMaskComplete
						copy_rect_nomask(rect_x1, rect_y1, rect_x2, rect_y2, rect_x3,
										rect_y3, blitter_colormode & 0x0F,
										(uint32_t*) ((u32) framebuffer
												+ blitter_src_offset),
										blitter_src_pitch, mask); // Mask in this case is minterm/opcode.
						break;
					}

					break;
				}

				case REG_ZZ_FILLTEMPLATE: {
					uint8_t draw_mode = blitter_colormode >> 8;
					uint8_t* tmpl_data = (uint8_t*) ((u32) framebuffer
							+ blitter_src_offset);
					set_fb((uint32_t*) ((u32) framebuffer + blitter_dst_offset),
							blitter_dst_pitch);

					uint8_t bpp = 2 * (blitter_colormode & 0xff);
					if (bpp == 0)
						bpp = 1;
					uint16_t loop_rows = 0;
					mask = zdata;

					if (zdata & 0x8000) {
						// pattern mode
						// TODO yoffset
						loop_rows = zdata & 0xff;
						mask = blitter_user1;
						blitter_src_pitch = 16;
						pattern_fill_rect((blitter_colormode & 0x0F), rect_x1,
								rect_y1, rect_x2, rect_y2, draw_mode, mask,
								rect_rgb, rect_rgb2, rect_x3, rect_y3, tmpl_data,
								blitter_src_pitch, loop_rows);
					}
					else {
						template_fill_rect((blitter_colormode & 0x0F), rect_x1,
								rect_y1, rect_x2, rect_y2, draw_mode, mask,
								rect_rgb, rect_rgb2, rect_x3, rect_y3, tmpl_data,
								blitter_src_pitch);
					}

					break;
				}

				case 0x50: { // Copy crap from scratch area
					for (int i = 0; i < rect_y1; i++) {
						memcpy	((uint32_t*) ((u32) framebuffer + framebuffer_pan_offset + (i * rect_x1)),
								 (uint32_t*) ((u32)Z3_SCRATCH_ADDR + (i * rect_x1)),
								 rect_x1);
					}
					break;
				}

				case 0x52: // Custom video mode param
					custom_vmode_param = zdata;
					break;

				case 0x54: { // Custom video mode data
					int *target = &preset_video_modes[custom_video_mode].hres;
					switch(custom_vmode_param) {
						case VMODE_PARAM_VRES: target = &preset_video_modes[custom_video_mode].vres; break;
						case VMODE_PARAM_HSTART: target = &preset_video_modes[custom_video_mode].hstart; break;
						case VMODE_PARAM_HEND: target = &preset_video_modes[custom_video_mode].hend; break;
						case VMODE_PARAM_HMAX: target = &preset_video_modes[custom_video_mode].hmax; break;
						case VMODE_PARAM_VSTART: target = &preset_video_modes[custom_video_mode].vstart; break;
						case VMODE_PARAM_VEND: target = &preset_video_modes[custom_video_mode].vend; break;
						case VMODE_PARAM_VMAX: target = &preset_video_modes[custom_video_mode].vmax; break;
						case VMODE_PARAM_POLARITY: target = &preset_video_modes[custom_video_mode].polarity; break;
						case VMODE_PARAM_MHZ: target = &preset_video_modes[custom_video_mode].mhz; break;
						case VMODE_PARAM_PHZ: target = &preset_video_modes[custom_video_mode].phz; break;
						case VMODE_PARAM_VHZ: target = &preset_video_modes[custom_video_mode].vhz; break;
						case VMODE_PARAM_HDMI: target = &preset_video_modes[custom_video_mode].hdmi; break;
						case VMODE_PARAM_MUL: target = &preset_video_modes[custom_video_mode].mul; break;
						case VMODE_PARAM_DIV: target = &preset_video_modes[custom_video_mode].div; break;
						case VMODE_PARAM_DIV2: target = &preset_video_modes[custom_video_mode].div2; break;
						default: break;
					}

					*target = zdata;
					break;
				}

				case 0x56: // Set custom video mode index
					custom_video_mode = zdata;
					break;

				case 0x58: // Set custom video mode without any questions asked.
					// This assumes that the custom video mode is 640x480 or higher resolution.
					video_mode_init(custom_video_mode, scalemode, colormode);
					break;

				case REG_ZZ_P2C: {
					uint8_t draw_mode = blitter_colormode >> 8;
					uint8_t planes = (zdata & 0xFF00) >> 8;
					uint8_t mask = (zdata & 0xFF);
					uint8_t layer_mask = blitter_user2;
					uint8_t* bmp_data = (uint8_t*) ((u32) framebuffer
							+ blitter_src_offset);

					set_fb((uint32_t*) ((u32) framebuffer + blitter_dst_offset),
							blitter_dst_pitch);

					p2c_rect(rect_x1, 0, rect_x2, rect_y2, rect_x3,
							rect_y3, draw_mode, planes, mask,
							layer_mask, blitter_src_pitch, bmp_data);
					break;
				}

				case REG_ZZ_P2D: {
					uint8_t draw_mode = blitter_colormode >> 8;
					uint8_t planes = (zdata & 0xFF00) >> 8;
					uint8_t mask = (zdata & 0xFF);
					uint8_t layer_mask = blitter_user2;
					uint8_t* bmp_data = (uint8_t*) ((u32) framebuffer
							+ blitter_src_offset);

					set_fb((uint32_t*) ((u32) framebuffer + blitter_dst_offset),
							blitter_dst_pitch);
					p2d_rect(rect_x1, 0, rect_x2, rect_y2, rect_x3,
							rect_y3, draw_mode, planes, mask, layer_mask, rect_rgb,
							blitter_src_pitch, bmp_data, (blitter_colormode & 0x0F));
					break;
				}

				case REG_ZZ_DRAWLINE: {
					uint8_t draw_mode = blitter_colormode >> 8;
					set_fb((uint32_t*) ((u32) framebuffer + blitter_dst_offset),
							blitter_dst_pitch);

					// rect_x3 contains the pattern. if all bits are set for both the mask and the pattern,
					// there's no point in passing non-essential data to the pattern/mask aware function.

					if (rect_x3 == 0xFFFF && zdata == 0xFF)
						draw_line_solid(rect_x1, rect_y1, rect_x2, rect_y2,
								blitter_user1, rect_rgb,
								(blitter_colormode & 0x0F));
					else
						draw_line(rect_x1, rect_y1, rect_x2, rect_y2,
								blitter_user1, rect_x3, rect_y3, rect_rgb,
								rect_rgb2, (blitter_colormode & 0x0F), zdata,
								draw_mode);
					break;
				}

				case REG_ZZ_INVERTRECT:
					set_fb((uint32_t*) ((u32) framebuffer + blitter_dst_offset),
							blitter_dst_pitch);
					invert_rect(rect_x1, rect_y1, rect_x2, rect_y2,
							zdata & 0xFF, blitter_colormode);
					break;

				case REG_ZZ_SET_SPLIT_POS:
					bgbuf_offset = blitter_src_offset;
					next_split_pos = zdata;

					video_formatter_write(split_pos, MNTVF_OP_REPORT_LINE);
					break;

				// Ethernet
				case REG_ZZ_ETH_TX:
					ethernet_send_result = ethernet_send_frame(zdata);
					//printf("SEND frame sz: %ld res: %d\n",zdata,ethernet_send_result);
					break;
				case REG_ZZ_ETH_RX: {
					//printf("RECV eth frame sz: %ld\n",zdata);
					int frfb = ethernet_receive_frame();
					mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG4, frfb);
					break;
				}
				case REG_ZZ_ETH_MAC_HI: {
					uint8_t* mac = ethernet_get_mac_address_ptr();
					mac[0] = (zdata & 0xff00) >> 8;
					mac[1] = (zdata & 0x00ff);
					break;
				}
				case REG_ZZ_ETH_MAC_HI2: {
					uint8_t* mac = ethernet_get_mac_address_ptr();
					mac[2] = (zdata & 0xff00) >> 8;
					mac[3] = (zdata & 0x00ff);
					break;
				}
				case REG_ZZ_ETH_MAC_LO: {
					uint8_t* mac = ethernet_get_mac_address_ptr();
					mac[4] = (zdata & 0xff00) >> 8;
					mac[5] = (zdata & 0x00ff);
					ethernet_update_mac_address();
					break;
				}
				case REG_ZZ_USBBLK_TX_HI: {
					usb_storage_write_block = ((u32) zdata) << 16;
					break;
				}
				case REG_ZZ_USBBLK_TX_LO: {
					usb_storage_write_block |= zdata;
					if (usb_storage_available) {
						usb_status = zz_usb_write_blocks(0, usb_storage_write_block, usb_read_write_num_blocks, (void*)USB_BLOCK_STORAGE_ADDRESS);
					} else {
						printf("[USB] TX but no storage available!\n");
					}
					break;
				}
				case REG_ZZ_USBBLK_RX_HI: {
					usb_storage_read_block = ((u32) zdata) << 16;
					break;
				}
				case REG_ZZ_USBBLK_RX_LO: {
					usb_storage_read_block |= zdata;
					if (usb_storage_available) {
						usb_status = zz_usb_read_blocks(0, usb_storage_read_block, usb_read_write_num_blocks, (void*)USB_BLOCK_STORAGE_ADDRESS);
					} else {
						printf("[USB] RX but no storage available!\n");
					}
					break;
				}
				case REG_ZZ_USB_STATUS: {
					//printf("[USB] write to status/blocknum register: %d\n", zdata);
					if (zdata==0) {
						// reset USB
						// FIXME memory leaks?
						//usb_storage_available = zz_usb_init();
					} else {
						// set number of blocks to read/write at once
						usb_read_write_num_blocks = zdata;
					}
					break;
				}
				case REG_ZZ_USB_BUFSEL: {
					//printf("[USB] select buffer: %d\n", zdata);
					usb_selected_buffer_block = zdata;
					mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG5, usb_selected_buffer_block);
					break;
				}
				case REG_ZZ_DEBUG: {
					debug_lowlevel = zdata;

					break;
				}

				// ARM core 2 execution
				case REG_ZZ_ARM_RUN_HI:
					arm_run_address = ((u32) zdata) << 16;
					break;
				case REG_ZZ_ARM_RUN_LO:
					// TODO checksum?
					arm_run_address |= zdata;

					*core1_addr = (uint32_t) core1_loop;
					core1_addr2[0] = 0xe3e0000f; // mvn	r0, #15  -- loads 0xfffffff0
					core1_addr2[1] = 0xe590f000; // ldr	pc, [r0] -- jumps to the address in that address

					printf("[ARM_RUN] %lx\n", arm_run_address);
					if (arm_run_address > 0) {
						core1_trampoline = (volatile void (*)(
								volatile struct ZZ9K_ENV*)) arm_run_address;
						printf("[ARM_RUN] signaling second core.\n");
						Xil_DCacheFlush();
						Xil_ICacheInvalidate();
						core2_execute = 1;
						Xil_DCacheFlush();
						Xil_ICacheInvalidate();
					} else {
						core1_trampoline = 0;
						core2_execute = 0;
					}

					// FIXME move this out of here
					// sequence to reset cpu1 taken from https://xilinx-wiki.atlassian.net/wiki/spaces/A/pages/18842504/XAPP1079+Latest+Information

					Xil_Out32(XSLCR_UNLOCK_ADDR, XSLCR_UNLOCK_CODE);
					uint32_t RegVal = Xil_In32(A9_CPU_RST_CTRL);
					RegVal |= A9_RST1_MASK;
					Xil_Out32(A9_CPU_RST_CTRL, RegVal);
					RegVal |= A9_CLKSTOP1_MASK;
					Xil_Out32(A9_CPU_RST_CTRL, RegVal);
					RegVal &= ~A9_RST1_MASK;
					Xil_Out32(A9_CPU_RST_CTRL, RegVal);
					RegVal &= ~A9_CLKSTOP1_MASK;
					Xil_Out32(A9_CPU_RST_CTRL, RegVal);
					Xil_Out32(XSLCR_LOCK_ADDR, XSLCR_LOCK_CODE);

					dmb();
					dsb();
					isb();
					asm("sev");
					break;
				case REG_ZZ_ARM_ARGC:
					arm_run_env.argc = zdata;
					break;
				case REG_ZZ_ARM_ARGV0:
					arm_run_env.argv[0] = ((u32) zdata) << 16;
					break;
				case REG_ZZ_ARM_ARGV1:
					arm_run_env.argv[0] |= zdata;
					printf("ARG0 set: %lx\n", arm_run_env.argv[0]);
					break;
				case REG_ZZ_ARM_ARGV2:
					arm_run_env.argv[1] = ((u32) zdata) << 16;
					break;
				case REG_ZZ_ARM_ARGV3:
					arm_run_env.argv[1] |= zdata;
					printf("ARG1 set: %lx\n", arm_run_env.argv[1]);
					break;
				case REG_ZZ_ARM_ARGV4:
					arm_run_env.argv[2] = ((u32) zdata) << 16;
					break;
				case REG_ZZ_ARM_ARGV5:
					arm_run_env.argv[2] |= zdata;
					printf("ARG2 set: %lx\n", arm_run_env.argv[2]);
					break;
				case REG_ZZ_ARM_ARGV6:
					arm_run_env.argv[3] = ((u32) zdata) << 16;
					break;
				case REG_ZZ_ARM_ARGV7:
					arm_run_env.argv[3] |= zdata;
					printf("ARG3 set: %lx\n", arm_run_env.argv[3]);
					break;
				case REG_ZZ_ARM_EV_CODE:
					arm_app_input_event_code = zdata;
					arm_app_input_event_serial++;
					arm_app_input_event_ack = 0;
					break;
				}
			}

			// ack the write, set bit 31 in register 0
			mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG0, (1 << 31));
			need_req_ack = 1;
		} else if (readreq) {
			uint32_t zaddr = mntzorro_read(MNTZ_BASE_ADDR, MNTZORRO_REG0);

			if (debug_lowlevel) {
				printf("READ: %08lx\n",zaddr);
			}
			u32 z3 = (zstate_raw & (1 << 25)); // TODO cache

			if (zaddr > 0x10000000) {
				printf("ERRR: illegal address %08lx\n", zaddr);
			}
			if (zaddr >= MNT_FB_BASE || zaddr >= MNT_REG_BASE + 0x2000) {
				u8* ptr = mem;

				if (zaddr >= MNT_FB_BASE) {
					// read from framebuffer / generic memory
					ptr = mem + zaddr - MNT_FB_BASE;
				} else if (zaddr < MNT_REG_BASE + 0x8000) {
					// 0x2000-0x7fff: FIXME: waste of address space
					// read from ethernet RX frame
					// disable INT6 interrupt
					mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG2, (1 << 30) | 0);
					ptr = (u8*) (ethernet_current_receive_ptr() + zaddr - (MNT_REG_BASE + 0x2000));
				} else if (zaddr < MNT_REG_BASE + 0xa000) {
					// 0x8000-0x9fff: read from TX frame (unusual)
					ptr = (u8*) (TX_FRAME_ADDRESS + zaddr - (MNT_REG_BASE + 0x8000));
					//printf("ETXF read: %08lx\n", (u32) ptr);
				} else if (zaddr < MNT_REG_BASE + 0x10000) {
					// 0xa000-0xafff: read from block device (usb storage)
					// TODO: this should be moved to DMA space?
					ptr = (u8*) (USB_BLOCK_STORAGE_ADDRESS + zaddr - (MNT_REG_BASE + 0xa000) + usb_selected_buffer_block * 512);
				}

				if (z3) {
					u32 b1 = ptr[0] << 24;
					u32 b2 = ptr[1] << 16;
					u32 b3 = ptr[2] << 8;
					u32 b4 = ptr[3];
					mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG1,
							b1 | b2 | b3 | b4);
				} else {
					u16 ubyte = ptr[0] << 8;
					u16 lbyte = ptr[1];
					mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG1,
							ubyte | lbyte);
				}
			} else if (zaddr >= MNT_REG_BASE) {
				// read ARM "register"
				uint32_t data = 0;
				uint32_t zaddr32 = zaddr & 0xffffffc;

				switch (zaddr32) {
					case REG_ZZ_VBLANK_STATUS:
						data = (zstate_raw & (1 << 21));
						break;
					case REG_ZZ_ARM_EV_SERIAL:
						data = (arm_app_output_event_serial << 16)
								| arm_app_output_event_code;
						arm_app_output_event_ack = 1;
						break;
					case REG_ZZ_ETH_MAC_HI: {
						uint8_t* mac = ethernet_get_mac_address_ptr();
						data = mac[0] << 24 | mac[1] << 16 | mac[2] << 8 | mac[3];
						break;
					}
					case REG_ZZ_ETH_MAC_LO: {
						uint8_t* mac = ethernet_get_mac_address_ptr();
						data = mac[4] << 24 | mac[5] << 16;
						break;
					}
					case REG_ZZ_ETH_TX:
						// FIXME this is probably wrong (doesn't need swapping?)
						data = (ethernet_send_result & 0xff) << 24
								| (ethernet_send_result & 0xff00) << 16;
						break;
					case REG_ZZ_FW_VERSION:
						data = (REVISION_MAJOR << 24 | REVISION_MINOR << 16);
						break;
					case REG_ZZ_USB_STATUS:
						data = usb_status << 16;
						break;
					case REG_ZZ_USB_CAPACITY: {
						if (usb_storage_available) {
							printf("[USB] query capacity: %lx\n",zz_usb_storage_capacity(0));
							data = zz_usb_storage_capacity(0);
						} else {
							printf("[USB] query capacity: no device.\n");
							data = 0;
						}
						break;
					}
					case REG_ZZ_TEMPERATURE: {
						// includes REG_ZZ_VOLTAGE_AUX in lower 16 bits
						data = (((uint32_t)(xadc_get_temperature()*10.0)) << 16) | ((uint32_t)(xadc_get_aux_voltage()*100.0));
						break;
					}
					case REG_ZZ_VOLTAGE_INT: {
						data = ((int16_t)(xadc_get_int_voltage()*100.0)) << 16;
						break;
					}
				}

				if (z3) {
					mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG1, data);
				} else {
					if (zaddr & 2) {
						// lower 16 bit
						mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG1, data);
					} else {
						// upper 16 bit
						mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG1, data >> 16);
					}
				}
			}

			// ack the read, set bit 30 in register 0
			mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG0, (1 << 30));
			need_req_ack = 2;
		} else {
			// there are no read/write requests, we can do other housekeeping

			int videocap_enabled = (zstate_raw & (1 << 23));
			int videocap_ntsc = (zstate_raw & (1<<22));

			// FIXME magic constant
			if (videocap_enabled && framebuffer_pan_offset >= 0xe00000) {
				if (sprite_enabled) {
					sprite_hide();
				}

				if (!videocap_enabled_old) {
					videocap_area_clear();
					videocap_ntsc_old = 0;
				}

				if (videocap_ntsc != videocap_ntsc_old) {
					// change between ntsc+pal
					videocap_area_clear();

					if (videocap_ntsc) {
						framebuffer_pan_offset = 0x00e00000;
						video_mode_init(ZZVMODE_720x480, 2, MNTVA_COLOR_32BIT);
					} else {
						// PAL
						reset_default_videocap_pan();
						framebuffer_pan_offset = default_pan_offset;
						video_mode_init(videocap_video_mode, 2, MNTVA_COLOR_32BIT);
					}
				}
				videocap_ntsc_old = videocap_ntsc;

				int interlace = !!(zstate_raw & (1 << 24));
				if (interlace != interlace_old) {
					// interlace has changed, we need to reconfigure vdma for the new screen height
					vmode_vdiv = 2;
					if (interlace) {
						vmode_vdiv = 1;
					}
					videocap_area_clear();
					init_vdma(vmode_hsize, vmode_vsize, 1, vmode_vdiv, (u32)framebuffer + framebuffer_pan_offset);
					video_formatter_valign();
					printf("videocap interlace mode changed to %d.\n", interlace);
				}
				interlace_old = interlace;
			}
			else {
				if(!sprite_enabled)
					sprite_enabled = 1;
			}

			if (videocap_enabled_old != videocap_enabled) {
				if (framebuffer_pan_offset >= 0xe00000) {
					videocap_area_clear();
				}
				videocap_enabled_old = videocap_enabled;
			}

			if (zstate == 0) {
				// RESET
				handle_amiga_reset();
			}
		}

		// TODO: potential hang, timeout?
		if (need_req_ack) {
			while (1) {
				// 1. fpga needs to respond to flag bit 31 or 30 going high (signals request fulfilled)
				// 2. it does that by clearing the request bit
				// 3. we read register 3 until request bit (31:write, 30:read) goes to 0 again
				//
				u32 zstate = mntzorro_read(MNTZ_BASE_ADDR, MNTZORRO_REG3);
				u32 writereq = (zstate & (1 << 31));
				u32 readreq = (zstate & (1 << 30));
				if (need_req_ack == 1 && !writereq)
					break;
				if (need_req_ack == 2 && !readreq)
					break;
				if ((zstate & 0xff) == 0)
					break; // reset
			}
			mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG0, 0);
			need_req_ack = 0;
		}

		// check for queued up ethernet frames
		int ethernet_backlog = ethernet_get_backlog();
		if (ethernet_backlog > 0 && backlog_nag_counter > 5000) {
			// interrupt amiga (trigger int6/2)
			mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG2, (1 << 30) | 1);
			usleep(1);
			mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG2, (1 << 30) | 0);
			backlog_nag_counter = 0;
		}

		if (interrupt_enabled && ethernet_backlog > 0) {
			backlog_nag_counter++;
		}
	}

	cleanup_platform();
	return 0;
}

void arm_exception_handler_id_reset(void *callback) {
	printf("id_reset: arm_exception_handler()!\n");
	while (1) {
	}
}

void arm_exception_handler_id_data_abort(void *callback) {
	printf("id_data_abort: arm_exception_handler()!\n");
	while (1) {
	}
}

void arm_exception_handler_id_prefetch_abort(void *callback) {
	printf("id_prefetch_abort: arm_exception_handler()!\n");
	while (1) {
	}
}

void arm_exception_handler(void *callback) {
	printf("arm_exception_handler()!\n");
	while (1) {
	}
}

void arm_exception_handler_illinst(void *callback) {
	printf("arm_exception_handler_illinst()!\n");
	while (1) {
	}
}

void DataAbort_InterruptHandler(void *InstancePtr) {
	while(1) {
	}
}
