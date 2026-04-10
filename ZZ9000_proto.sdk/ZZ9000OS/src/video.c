#include <stdint.h>
#include <stdio.h>
#include "video.h"
#include "mntzorro.h"
#include "interrupt.h"
#include "xaxivdma.h"
#include "xclk_wiz.h"
#include "hdmi.h"
#include <sleep.h>
#include "xil_cache_l.h"

#define VDMA_DEVICE_ID	XPAR_AXIVDMA_0_DEVICE_ID

static struct ZZ_VIDEO_STATE vs;
static XAxiVdma vdma;
static XClk_Wiz clkwiz;

extern int interrupt_enabled_vblank;

struct zz_video_mode preset_video_modes[ZZVMODE_NUM] = {
    //   HRES       VRES    HSTART  HEND    HMAX    VSTART  VEND    VMAX    POLARITY    MHZ     PIXELCLOCK HZ   VERTICAL HZ     HDMI    MUL/DIV/DIV2
    {    1280,      720,    1390,   1430,   1650,   725,    730,    750,    0,          75,     75000000,       60,             0,      15, 1, 20 },
    {    800,       600,    840,    968,    1056,   601,    605,    628,    0,          40,     40000000,       60,             0,      14, 1, 35 },
    {    640,       480,    656,    752,    800,    490,    492,    525,    0,          25,     25175000,       60,             0,      15, 1, 60 },
    {    1024,      768,    1048,   1184,   1344,   771,    777,    806,    0,          65,     65000000,       60,             0,      13, 1, 20 },
    {    1280,      1024,   1328,   1440,   1688,   1025,   1028,   1066,   0,          108,    108000000,      60,             0,      54, 5, 10 },
    {    1920,      1080,   2008,   2052,   2200,   1084,   1089,   1125,   0,          150,    150000000,      60,             0,      15, 1, 10 },
    {    720,       576,    732,    796,    864,    581,    586,    625,    1,          27,     27000000,       50,             0,      45, 2, 83 },
    {    1920,      1080,   2448,   2492,   2640,   1084,   1089,   1125,   0,          150,    150000000,      50,             0,      15, 1, 10 },
    {    720,       480,    736,    768,    800,    490,    492,    525,    0,          25,     25175000,       60,             0,      19, 1, 75 },
    {    640,       512,    840,    968,    1056,   601,    605,    628,    0,          40,     40000000,       60,             0,      14, 1, 35 },
	{	 1600,		1200,	1704,	1880,	2160,	1201,	1204,	1242,	0,			161,	16089999,		60,				0,		21, 1, 13 },
	{	 2560,		1440,	2680,	2944,	3328,	1441,	1444,	1465,	0,			146,	15846000,		30,				0,		41, 2, 14 },
	{    720,       576,    732,    796,    864,    581,    586,    625,    1,          27,     27000000,       50,             0,      31, 1,115 }, // 720x576 non-standard VSync (PAL Amiga)
	{    720,       480,    720,    752,    800,    490,    492,    525,    0,          25,     25175000,       60,             0,      61, 5, 49 }, // 720x480 non-standard VSync (PAL Amiga)
	{    720,       576,    732,    796,    864,    581,    586,    625,    1,          27,     27000000,       50,             0,      59, 7, 31 }, // 720x576 non-standard VSync (NTSC Amiga)
	{    720,       480,    720,    752,    800,    490,    492,    525,    0,          25,     25175000,       60,             0,      37, 3, 49 }, // 720x480 non-standard VSync (NTSC Amiga)
    {    640,       400,    656,    752,    800,    490,    492,    525,    0,          25,     25175000,       60,             0,      15, 1, 60 },
    {    1920,      800,    2024,   2224,   2528,   801,    804,    828,    0,          125,    125000000,      60,             0,      15, 1, 12 },
    // The final entry here is the custom video mode, accessible through registers for debug purposes.
    {    1280,      720,    1390,   1430,   1650,   725,    730,    750,    0,          75,     75000000,       60,             0,      15, 1, 20 },
};

uint32_t sprite_buf[32 * 48];
uint8_t sprite_clipped = 0;
int16_t sprite_clip_x = 0, sprite_clip_y = 0;

int sprite_request_update_pos = 0;
int sprite_request_update_data = 0;
int sprite_request_show = 0;
int sprite_request_hide = 0;
int sprite_request_pos_x = 0;
int sprite_request_pos_y = 0;

void _update_hw_sprite_pos(int16_t x, int16_t y);
void _clip_hw_sprite(int16_t offset_x, int16_t offset_y);

// FIXME integrate with memory map
static int default_pan_offset_pal = 0x00e00000;
static int default_pan_offset_ntsc = 0x00e00000;
static int default_pan_offset_pal_800x600 = 0x00dff2f8;

static int isr_flush_count = 0;
int vblank_count = 0;

struct ZZ_VIDEO_STATE* video_get_state() {
	return &vs;
}

struct ZZ_VIDEO_STATE* video_init() {
	vs.framebuffer = (u32*) FRAMEBUFFER_ADDRESS;

	// default to more compatible 60hz mode
	vs.videocap_video_mode = ZZVMODE_800x600;
	vs.video_mode = ZZVMODE_800x600 | 2 << 12 | MNTVA_COLOR_32BIT << 8;
	vs.colormode = 0;

	video_reset();

	return video_get_state();
}

void video_reset() {
	vs.videocap_enabled_old = 0;
	vs.framebuffer_pan_width = 0;
	vs.framebuffer_pan_offset = default_pan_offset_pal_800x600;
	vs.split_request_pos = 0;

	vs.sprite_colors[0] = 0x00ff00ff;
	vs.sprite_colors[1] = 0x00000000;
	vs.sprite_colors[2] = 0x00000000;
	vs.sprite_colors[3] = 0x00000000;

	vs.sprite_width = 16;
	vs.sprite_height = 16;

	sprite_request_hide = 1;
}

uint8_t stride_div = 1;

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

	//printf("VDMA MM2S DRE: %d\n", vdma.HasMm2SDRE);
	//printf("VDMA Config MM2S DRE: %d\n", Config->HasMm2SDRE);

	u32 stride = hsize * (Config->Mm2SStreamWidth >> 3);
	if (vs.framebuffer_pan_width != 0 && vs.framebuffer_pan_width != (hsize / hdiv)) {
		stride = (vs.framebuffer_pan_width * (Config->Mm2SStreamWidth >> 3)) * stride_div;
	}

	XAxiVdma_DmaSetup ReadCfg;

	//printf("VDMA HDIV: %d VDIV: %d\n", hdiv, vdiv);

	ReadCfg.VertSizeInput = vsize / vdiv;
	ReadCfg.HoriSizeInput = (hsize * (Config->Mm2SStreamWidth >> 3)) / hdiv; // note: changing this breaks the output
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

void init_ns_video_mode(uint32_t mode_num) {
	printf("init_ns_video_mode(%lu)\n", mode_num);
	if (mode_num == ZZVMODE_720x576) {
		video_mode_init(ZZVMODE_720x576_NS_PAL + vs.scandoubler_mode_adjust, 2, MNTVA_COLOR_32BIT);
	} else {
		video_mode_init(ZZVMODE_720x480_NS_PAL + vs.scandoubler_mode_adjust, 2, MNTVA_COLOR_32BIT);
	}
}

void fb_fill(uint32_t offset) {
	memset(vs.framebuffer + offset, 0, 1280 * 1024 * 4);
}

void videocap_area_clear() {
	fb_fill(0x00dff000 / 4);
}

#define VF_DLY ;

// ONLY isr_video is allowed to call this!
void video_formatter_valign() {
	// vertical alignment
	mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG3, 1);
	VF_DLY;
	mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG2, 0x80000000 + 0x5); // OP_VSYNC
	VF_DLY;
	mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG2, 0x80000000); // NOP
	VF_DLY;
	mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG2, 0); // clear
	VF_DLY;
	mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG3, 0); // unlock access, NOP
	VF_DLY;
}

// ONLY isr_video is allowed to call this!
void video_formatter_write(uint32_t data, uint16_t op) {
	mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG3, data);
	VF_DLY;
	mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG2, 0x80000000 | op); // OP_MAX (vmax | hmax)
	VF_DLY;
	mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG2, 0x80000000); // NOP
	VF_DLY;
	mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG2, 0); // clear
	VF_DLY;
	mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG3, 0); // unlock access, NOP
	VF_DLY;
}

// interrupt service routine for IRQ_F2P[0:0]
// vblank + raster position interrupt
void isr_video(void *dummy) {
	u32 zstate = mntzorro_read(MNTZ_BASE_ADDR, MNTZORRO_REG3);

	int vblank = (zstate & (1 << 21));
	int videocap_enabled = (zstate & (1 << 23));
	int videocap_ntsc = (zstate & (1 << 22));
	int interlace = !!(zstate & (1 << 24));

	if (!videocap_enabled) {
		if (!vblank) {
			// if this is not the vblank interrupt, set up the split buffer
			// TODO: VDMA doesn't seem to like switching buffers in the middle of a frame.
			// the first line after a switch contains an extraneous word, so we end up
			// with up to 4 pixels of the other buffer in the first line
			if (vs.split_pos != 0) {
				if (vs.card_feature_enabled[CARD_FEATURE_SECONDARY_PALETTE]) {
					video_formatter_write(1, MNTVF_OP_PALETTE_SEL);
				}
				init_vdma(vs.vmode_hsize, vs.vmode_vsize, vs.vmode_hdiv, vs.vmode_vdiv,
						(u32)vs.framebuffer + vs.bgbuf_offset);
			}
		} else {
			// if this is the vblank interrupt, set up the "normal" buffer in split mode
			if (vs.card_feature_enabled[CARD_FEATURE_SECONDARY_PALETTE]) {
				video_formatter_write(0, MNTVF_OP_PALETTE_SEL);
			}
			init_vdma(vs.vmode_hsize, vs.vmode_vsize, vs.vmode_hdiv, vs.vmode_vdiv,
					(u32)vs.framebuffer + vs.framebuffer_pan_offset);
		}
	} else {
		// FIXME magic constant
		if (vs.framebuffer_pan_offset >= 0x00dff000) {
			// videocap is enabled and
			// we are looking at the videocap area
			// so set up the right mode for it

			int videocap_reset = 0;

			if (!vs.videocap_enabled_old) {
				videocap_area_clear();
				// force mode cleanup
				videocap_reset = 1;
			}

			if (videocap_ntsc != vs.videocap_ntsc_old || videocap_reset) {
				// change between ntsc+pal
				videocap_area_clear();

				// hide sprite
				sprite_request_hide = 1;

				if (videocap_ntsc) {
					// NTSC
					printf("videocap: ntsc\n");
					vs.framebuffer_pan_width = 0;
					vs.framebuffer_pan_offset = default_pan_offset_ntsc;
					if (vs.card_feature_enabled[CARD_FEATURE_NONSTANDARD_VSYNC]) {
						init_ns_video_mode(ZZVMODE_720x480);
					} else {
						video_mode_init(ZZVMODE_720x480, 2, MNTVA_COLOR_32BIT);

						// use this if there are problems with 720x480
						//video_mode_init(ZZVMODE_800x600, 2, MNTVA_COLOR_32BIT);
					}
				} else {
					// PAL
					printf("videocap: pal\n");
					vs.framebuffer_pan_width = 0;
					if (vs.videocap_video_mode == ZZVMODE_800x600) {
						vs.framebuffer_pan_offset = default_pan_offset_pal_800x600;
					} else {
						vs.framebuffer_pan_offset = default_pan_offset_pal;
					}
					if (vs.videocap_video_mode == ZZVMODE_720x576 && vs.card_feature_enabled[CARD_FEATURE_NONSTANDARD_VSYNC]) {
						init_ns_video_mode(ZZVMODE_720x576);
					} else {
						video_mode_init(vs.videocap_video_mode, 2, MNTVA_COLOR_32BIT);
					}
				}
				videocap_reset = 1;
			}

			if (interlace != vs.interlace_old || videocap_reset) {
				// interlace has changed, we need to reconfigure vdma for the new screen height
				vs.vmode_vdiv = 2;
				if (interlace) {
					vs.vmode_vdiv = 1;
				}
				videocap_area_clear();
				init_vdma(vs.vmode_hsize, vs.vmode_vsize, 1, vs.vmode_vdiv,
						(u32)vs.framebuffer + vs.framebuffer_pan_offset);
				video_formatter_valign();
				printf("videocap interlace mode changed to %d.\n", interlace);
			}

			vs.interlace_old = interlace;
			vs.videocap_ntsc_old = videocap_ntsc;
			vs.videocap_enabled_old = videocap_enabled;
		} else {
			// not looking at the videocap area
			vs.videocap_enabled_old = 0;
			vs.videocap_ntsc_old = -1;
			vs.interlace_old = -1;
		}
	}

	// on vblanks, handle arm cache flush, amiga interrupts and sprites
	if (!vblank || (vs.split_pos == 0)) {
		// flush the data caches synchronized to full frames
		Xil_L1DCacheFlush();
		Xil_L2CacheFlush();
		isr_flush_count = 0;

		if (sprite_request_show) {
			vs.sprite_showing = 1;
			sprite_request_show = 0;
		}

		if (sprite_request_update_data) {
			_clip_hw_sprite(0, 0);
			sprite_request_update_data = 0;
		}

		if (sprite_request_update_pos) {
			_update_hw_sprite_pos(sprite_request_pos_x, sprite_request_pos_y);
			video_formatter_write((vs.sprite_y_adj << 16) | vs.sprite_x_adj, MNTVF_OP_SPRITE_XY);
			sprite_request_update_pos = 0;
		}

		if (sprite_request_hide) {
			vs.sprite_x = 2000;
			vs.sprite_y = 2000;
			video_formatter_write((vs.sprite_y << 16) | vs.sprite_x, MNTVF_OP_SPRITE_XY);
			sprite_request_hide = 0;
			vs.sprite_showing = 0;
		}

		// handle screen dragging
		if (vs.split_request_pos != vs.split_pos) {
			int scale = 1;
			if (vs.scalemode & 2) scale = 2;
			vs.split_pos = vs.split_request_pos * scale;
			video_formatter_write(vs.split_pos, MNTVF_OP_REPORT_LINE);
		}
	}

	vblank_count++;

	// signal vblank interrupt to Amiga for P96 BIF_VBLANKINTERRUPT support
	if (vblank && interrupt_enabled_vblank) {
		amiga_interrupt_set(AMIGA_INTERRUPT_VBLANK);
	}
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

void pixelclock_init_2(struct zz_video_mode *mode) {
	XClk_Wiz_Config conf;
	XClk_Wiz_CfgInitialize(&clkwiz, &conf, XPAR_CLK_WIZ_0_BASEADDR);

	u32 mul = mode->mul;
	u32 div = mode->div;
	u32 otherdiv = mode->div2;

	XClk_Wiz_WriteReg(XPAR_CLK_WIZ_0_BASEADDR, 0x200, (mul << 8) | div);
	XClk_Wiz_WriteReg(XPAR_CLK_WIZ_0_BASEADDR, 0x208, otherdiv);

	// load configuration
	XClk_Wiz_WriteReg(XPAR_CLK_WIZ_0_BASEADDR, 0x25C, 0x00000003);
	//XClk_Wiz_WriteReg(XPAR_CLK_WIZ_0_BASEADDR,  0x25C, 0x00000001);
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
	hdmi_ctrl_init(mode);
	init_vdma(mode->hres, mode->vres, hdiv, vdiv, (u32)vs.framebuffer + vs.framebuffer_pan_offset);
}

void video_mode_init(int mode, int scalemode, int colormode) {
	printf("video_mode_init: %d color: %d scale: %d\n", mode, colormode, scalemode);

	// reset interlace tracking
	vs.interlace_old = -1;
	// remember mode
	vs.video_mode = mode;
	vs.scalemode = scalemode;
	vs.colormode = colormode;

	int hdiv = 1, vdiv = 1;
	stride_div = 1;

	if (scalemode & 1) {
		hdiv = 2;
		stride_div = 2;
	}
	if (scalemode & 2)
		vdiv = 2;

	// 8 bit
	if (colormode == MNTVA_COLOR_8BIT)
		hdiv *= 4;

	if (colormode == MNTVA_COLOR_16BIT565 || colormode == MNTVA_COLOR_15BIT)
		hdiv *= 2;

	struct zz_video_mode *vmode = &preset_video_modes[mode];

	video_system_init(vmode, hdiv, vdiv);

	video_formatter_init(scalemode, colormode,
			vmode->hres, vmode->vres,
			vmode->hmax, vmode->vmax,
			vmode->hstart, vmode->hend,
			vmode->vstart, vmode->vend,
			vmode->polarity);

	// FIXME ???
	vs.vmode_hsize = vmode->hres;
	vs.vmode_vsize = vmode->vres;
	vs.vmode_vdiv = vdiv;
	vs.vmode_hdiv = hdiv;
}

void update_hw_sprite(uint8_t *data)
{
	uint8_t cur_bit = 0x80;
	uint8_t cur_color = 0, out_pos = 0, iter_offset = 0;
	uint8_t cur_bytes[8];
	uint32_t *colors = vs.sprite_colors;
	uint16_t w = vs.sprite_width;
	uint16_t h = vs.sprite_height;
	uint8_t line_pitch = (w / 8) * 2;

	for (uint8_t y_line = 0; y_line < h; y_line++) {
		if (w <= 16) {
			cur_bytes[0] = data[y_line * line_pitch];
			cur_bytes[1] = data[(y_line * line_pitch) + 2];
			cur_bytes[2] = data[(y_line * line_pitch) + 1];
			cur_bytes[3] = data[(y_line * line_pitch) + 3];
		}
		else {
			cur_bytes[0] = data[y_line * line_pitch];
			cur_bytes[1] = data[(y_line * line_pitch) + 4];
			cur_bytes[2] = data[(y_line * line_pitch) + 1];
			cur_bytes[3] = data[(y_line * line_pitch) + 5];
			cur_bytes[4] = data[(y_line * line_pitch) + 2];
			cur_bytes[5] = data[(y_line * line_pitch) + 6];
			cur_bytes[6] = data[(y_line * line_pitch) + 3];
			cur_bytes[7] = data[(y_line * line_pitch) + 7];
		}

		while (out_pos < 8) {
			for (uint8_t i = 0; i < line_pitch; i += 2) {
				cur_color = (cur_bytes[i] & cur_bit) ? 1 : 0;
				if (cur_bytes[i + 1] & cur_bit) cur_color += 2;

				sprite_buf[(y_line * 32) + out_pos + iter_offset] = colors[cur_color] & 0x00ffffff;
				iter_offset += 8;
			}

			out_pos++;
			cur_bit >>= 1;
			iter_offset = 0;
		}
		cur_bit = 0x80;
		out_pos = 0;
	}

	sprite_request_update_data = 1;
}

void update_hw_sprite_clut(uint8_t *data_, uint8_t *colors, uint16_t w, uint16_t h, uint8_t keycolor)
{
	uint8_t *data = data_;
	uint8_t color[4];

	for (int y = 0; y < h && y < 48; y++) {
		for (int x = 0; x < w && x < 32; x++) {
			if (data[x] == keycolor) {
				*((uint32_t *)color) = 0x00ff00ff;
			}
			else {
				color[0] = colors[(data[x] * 3)+2];
				color[1] = colors[(data[x] * 3)+1];
				color[2] = colors[(data[x] * 3)];
				color[3] = 0x00;
				if (*((uint32_t *)color) == 0x00FF00FF)
					*((uint32_t *)color) = 0x00FE00FE;
			}
			sprite_buf[(y * 32) + x] = *((uint32_t *)color);
		}
		data += w;
	}

	sprite_request_update_data = 1;
}

void clear_hw_sprite()
{
	for (uint16_t i = 0; i < 32 * 48; i++) {
		sprite_buf[i] = 0x00ff00ff;
	}
	//sprite_request_update_data = 1;
}

void _clip_hw_sprite(int16_t offset_x, int16_t offset_y)
{
	uint16_t xo = 0, yo = 0;
	if (offset_x < 0)
		xo = -offset_x;
	if (offset_y < 0)
		yo = -offset_y;

	for (int y = 0; y < 48; y++) {
		//printf("CLIP %02d: ",y);
		for (int x = 0; x < 32; x++) {
			video_formatter_write((y * 32) + x, 14);
			if (x < 32 - xo && y < 48 - yo) {
				//printf("%06lx", sprite_buf[((y + yo) * 32) + (x + xo)] & 0x00ffffff);
				video_formatter_write(sprite_buf[((y + yo) * 32) + (x + xo)] & 0x00ffffff, 15);
			} else {
				//printf("%06lx", 0x00ff00ff);
				video_formatter_write(0x00ff00ff, 15);
			}
		}
		//printf("\n");
	}
}

void _update_hw_sprite_pos(int16_t x, int16_t y) {
	vs.sprite_x = x - vs.sprite_x_offset + 1;
	// horizontally doubled mode
	if (vs.scalemode & 1)
		vs.sprite_x_adj = (vs.sprite_x * 2) + 1;
	else
		vs.sprite_x_adj = vs.sprite_x + 2;

	vs.sprite_y = y - vs.sprite_y_offset + 1;

	// vertically doubled mode
	if (vs.scalemode & 2)
		vs.sprite_y_adj = vs.sprite_y * 2;
	else
		vs.sprite_y_adj = vs.sprite_y;

	if (vs.sprite_x < 0 || vs.sprite_y < 0) {
		if (sprite_clip_x != vs.sprite_x || sprite_clip_y != vs.sprite_y) {
			_clip_hw_sprite((vs.sprite_x < 0) ? vs.sprite_x : 0, (vs.sprite_y < 0) ? vs.sprite_y : 0);
		}
		sprite_clipped = 1;
		if (vs.sprite_x < 0) {
			vs.sprite_x_adj = 0;
			sprite_clip_x = vs.sprite_x;
		}
		if (vs.sprite_y < 0) {
			vs.sprite_y_adj = 0;
			sprite_clip_y = vs.sprite_y;
		}
	}
	else if (sprite_clipped && vs.sprite_x >= 0 && vs.sprite_y >= 0) {
		_clip_hw_sprite(0, 0);
		sprite_clipped = 0;
	}
}

void update_hw_sprite_pos() {
	sprite_request_pos_x = vs.sprite_x_base;
	sprite_request_pos_y = vs.sprite_y_base;
	sprite_request_update_pos = 1;
}

void hw_sprite_show(int show) {
	if (show) {
		sprite_request_show = 1;
	} else {
		sprite_request_hide = 1;
	}
}

struct zz_video_mode* get_custom_video_mode_ptr(int custom_video_mode) {
	return &preset_video_modes[custom_video_mode];
}
