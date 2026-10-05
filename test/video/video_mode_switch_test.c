/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Run the production mode transaction against simulated MMIO/I2C peripherals.
 * A same-timing RTG/native switch must not remove the monitor's signal; real
 * timing changes and a lost PLL lock must still take the retraining path.
 * The staged custom-modeline transaction (zz_custom_mode.h) is exercised
 * end to end: rejection paths leave the hardware untouched, a failed PLL
 * lock replays the previous output, and retries succeed.
 */
#include <assert.h>
#include <string.h>
#include "xparameters.h"
#include "xil_printf.h"
#include "xiicps.h"
#include "../../ZZ9000_proto.sdk/ZZ9000OS/src/video.c"

static unsigned clock_reloads, tmds_interruptions;
static unsigned delay_us;
static uint32_t clock_locked = 1, clock_load;
static uint32_t clock_mul_div, clock_div2;
static unsigned clock_fail_locks;
static uint8_t transmitter[256], i2c_register;
static uint32_t formatter_data, formatter_ops[32];
static XAxiVdma_DmaSetup dma_setup;
static unsigned dma_starts;
static uint32_t video_irq_enabled = 1;
static unsigned inject_native_irq, deferred_native_irqs;
static uint32_t videocap_zstate;
static uint32_t videocap_live_geometry_reg;
static uint32_t zorro_ram_write_data;

static uint32_t videocap_live_geometry(uint16_t words, uint16_t rows)
{
	return MNTZORRO_REG2_LIVE_GEOMETRY_MAGIC |
		(((uint32_t)words & MNTZORRO_REG2_LIVE_GEOMETRY_WORDS_MASK) <<
		 MNTZORRO_REG2_LIVE_GEOMETRY_WORDS_SHIFT) |
		((uint32_t)rows & MNTZORRO_REG2_LIVE_GEOMETRY_ROWS_MASK);
}
static int dma_cfg_initialize_status;
static int dma_config_status;
static int dma_address_status;
static int dma_start_status;
static uint32_t capture_framebuffer[6 * 1280 * 1024];
int interrupt_enabled_vblank;
static struct zz_config test_config = {
	.videocap_output_profile = ZZ_VIDEOCAP_OUTPUT_FULL_60,
};
const struct zz_config *zz_config_get(void)
{
	return &test_config;
}
void Xil_L1DCacheFlush(void) {}
void Xil_L2CacheFlush(void) {}
uint32_t overlay_present_bufpos(struct ZZ_VIDEO_STATE *state)
{
	return (uint32_t)state->framebuffer + state->framebuffer_pan_offset;
}
int overlay_scanout_active(void) { return 0; }
void overlay_scanout_released(void) {}
void overlay_vblank_rearm(void) {}
void overlay_vblank_cache_flushed(void) {}
void amiga_interrupt_set(uint32_t bit) { (void)bit; }

void test_xil_out32(uintptr_t address, uint32_t value)
{
	if (address == XPAR_CLK_WIZ_0_BASEADDR + CLK_WIZ_RECONFIG_OFFSET) {
		++clock_reloads;
		if (clock_fail_locks > 0) {
			--clock_fail_locks;
			clock_locked = 0;
			clock_load = CLK_WIZ_RECONFIG_LOAD;
		} else {
			clock_locked = 1;
			clock_load = 0;
		}
	} else if (address == XPAR_CLK_WIZ_0_BASEADDR + 0x200) {
		clock_mul_div = value;
	} else if (address == XPAR_CLK_WIZ_0_BASEADDR + 0x208) {
		clock_div2 = value;
	} else if (address == MNTZ_BASE_ADDR + MNTZORRO_REG3) {
		formatter_data = value;
	} else if (address == MNTZ_BASE_ADDR + MNTZORRO_REG2 &&
	           (value & 0x80000000U)) {
		formatter_ops[value & 31U] = formatter_data;
	}
}

uint32_t test_xil_in32(uintptr_t address)
{
	if (address == XPAR_CLK_WIZ_0_BASEADDR + CLK_WIZ_STATUS_OFFSET)
		return clock_locked;
	if (address == XPAR_CLK_WIZ_0_BASEADDR + CLK_WIZ_RECONFIG_OFFSET)
		return clock_load;
	if (address == MNTZ_BASE_ADDR + MNTZORRO_REG3)
		return videocap_zstate;
	if (address == MNTZ_BASE_ADDR + MNTZORRO_REG2)
		return videocap_live_geometry_reg;
	if (address == MNTZ_BASE_ADDR + MNTZORRO_REG1)
		return zorro_ram_write_data;
	return 0;
}

uint32_t smp_local_irq_save(void) { return 0; }
void smp_local_irq_restore(uint32_t state) { (void)state; }
int videocap_stats_hw_present(void) { return 1; }
uint32_t video_interrupt_pause(void)
{
	uint32_t enabled = video_irq_enabled;
	video_irq_enabled = 0;
	return enabled;
}
void video_interrupt_restore(uint32_t enabled)
{
	if (enabled)
		video_irq_enabled = 1;
}
void usleep(unsigned long useconds)
{
	delay_us += useconds;
	if (inject_native_irq && clock_load &&
	    useconds == CLK_WIZ_LOCK_POLL_US) {
		inject_native_irq = 0;
		if (video_irq_enabled)
			init_videocap_video_mode(0, 1,
				ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60, 0);
		else
			++deferred_native_irqs;
	}
}

u32 XClk_Wiz_CfgInitialize(XClk_Wiz *instance, XClk_Wiz_Config *config,
		UINTPTR address)
{
	(void)instance; (void)config; (void)address;
	return XST_SUCCESS;
}

XAxiVdma_Config *XAxiVdma_LookupConfig(u16 id)
{
	static XAxiVdma_Config config;
	(void)id;
	return &config;
}
int XAxiVdma_CfgInitialize(XAxiVdma *instance, XAxiVdma_Config *config,
		UINTPTR address)
{
	(void)instance; (void)config; (void)address;
	return dma_cfg_initialize_status;
}
int XAxiVdma_DmaConfig(XAxiVdma *instance, u16 direction,
		XAxiVdma_DmaSetup *config)
{
	(void)instance; (void)direction;
	dma_setup = *config;
	return dma_config_status;
}
int XAxiVdma_DmaSetBufferAddr(XAxiVdma *instance, u16 direction,
		UINTPTR *addresses)
{
	(void)instance; (void)direction; (void)addresses;
	return dma_address_status;
}
int XAxiVdma_DmaStart(XAxiVdma *instance, u16 direction)
{
	(void)instance; (void)direction;
	++dma_starts;
	return dma_start_status;
}

XIicPs_Config *XIicPs_LookupConfig(u16 id)
{
	static XIicPs_Config config;
	(void)id;
	return &config;
}
s32 XIicPs_CfgInitialize(XIicPs *instance, XIicPs_Config *config, u32 address)
{
	(void)instance; (void)config; (void)address;
	return XST_SUCCESS;
}
s32 XIicPs_BusIsBusy(XIicPs *instance) { (void)instance; return 0; }
s32 XIicPs_SelfTest(XIicPs *instance) { (void)instance; return XST_SUCCESS; }
s32 XIicPs_SetSClk(XIicPs *instance, u32 rate)
{
	(void)instance; (void)rate;
	return XST_SUCCESS;
}
s32 XIicPs_MasterSendPolled(XIicPs *instance, u8 *data, s32 count, u16 address)
{
	(void)instance; (void)address;
	i2c_register = data[0];
	if (count == 2) {
		transmitter[i2c_register] = data[1];
		if (i2c_register == 0x1a && (data[1] & 0x10))
			++tmds_interruptions;
	}
	return XST_SUCCESS;
}
s32 XIicPs_MasterRecvPolled(XIicPs *instance, u8 *data, s32 count, u16 address)
{
	(void)instance; (void)count; (void)address;
	data[0] = transmitter[i2c_register];
	return XST_SUCCESS;
}

/* Stage a valid 960x720 modeline (issue97 target) through the word
 * protocol: 53 MHz pixel clock from PLL 53/4/25 (VCO 1325 MHz), ~60 Hz.
 * One field id can be skipped to build an incomplete request. */
static void stage_custom_960(uint16_t hstart, uint16_t skip)
{
	static const struct { uint16_t param, value; } fields[] = {
		{ ZZ_CUSTOM_HRES, 960 },     { ZZ_CUSTOM_VRES, 720 },
		{ ZZ_CUSTOM_HSTART, 0 },     { ZZ_CUSTOM_HEND, 1040 },
		{ ZZ_CUSTOM_HTOTAL, 1188 },  { ZZ_CUSTOM_VSTART, 729 },
		{ ZZ_CUSTOM_VEND, 733 },     { ZZ_CUSTOM_VTOTAL, 746 },
		{ ZZ_CUSTOM_POLARITY, 0 },   { ZZ_CUSTOM_MUL, 53 },
		{ ZZ_CUSTOM_DIV, 4 },        { ZZ_CUSTOM_DIV2, 25 },
	};

	video_custom_select(ZZ_CUSTOM_MODE_SLOT);
	for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		if (fields[i].param == skip)
			continue;
		video_custom_set_param(fields[i].param);
		video_custom_set_value(
			fields[i].param == ZZ_CUSTOM_HSTART ? hstart :
			fields[i].value);
	}
}

static uint16_t commit_custom(uint32_t color)
{
	return video_custom_commit(
		ZZ_CUSTOM_MODE_SLOT | (uint16_t)(color << 8));
}

static void clear_measurements(void)
{
	clock_reloads = tmds_interruptions = delay_us = dma_starts = 0;
	dma_cfg_initialize_status = dma_config_status = dma_address_status =
		dma_start_status = XST_SUCCESS;
}
static void geometry_native_vblank(void)
{
	for (unsigned i = 0; i < VIDEO_VIDEOCAP_MODE_STABLE_VBLANKS; ++i)
		isr_video(NULL);
}

static void assert_geometry_pending(uint16_t request_serial)
{
	assert(video_videocap_geometry_value(
		ZZ_CONFIG_KEY_VCAP_GEOMETRY_STATUS, NULL) &
		ZZ_VCAP_GEOMETRY_STATUS_PENDING);
	assert(video_videocap_geometry_value(
		ZZ_CONFIG_KEY_VCAP_GEOMETRY_REQUEST_SERIAL, NULL) ==
		request_serial);
	assert(video_videocap_geometry_value(
		ZZ_CONFIG_KEY_VCAP_GEOMETRY_APPLIED_SERIAL, NULL) !=
		request_serial);
}

int main(void)
{
	/* The persisted boot override follows the viewport bit: a legacy
	 * bitstream keeps the automatic window. */
	test_config.videocap_width = 640;
	test_config.videocap_height = 240;
	test_config.videocap_width_present = 1;
	test_config.videocap_height_present = 1;
	video_init();
	assert(vs.videocap_width_override == 0 &&
	       vs.videocap_height_override == 0);
	assert(vs.videocap_geometry_requested_width == 0 &&
	       vs.videocap_geometry_requested_height == 0);
	videocap_zstate = MNTZORRO_STATUS_VCAP_VIEWPORT;
	video_init();
	assert(vs.videocap_width_override == 640 &&
	       vs.videocap_height_override == 240);
	assert(vs.videocap_geometry_requested_width == 640 &&
	       vs.videocap_geometry_requested_height == 240);
	assert(vs.videocap_geometry_request_serial == 1);
	/* Leave the automatic window and the empty config for the rest. */
	test_config.videocap_width_present = 0;
	test_config.videocap_height_present = 0;
	videocap_zstate = 0;
	video_init();
	/* The geometry section below counts setter calls from a boot that
	 * never carried an override. */
	vs.videocap_geometry_request_serial = 0;

	/* Cold start must initialize the physical output. */
	video_mode_init(ZZVMODE_1920x1080_60, 0, MNTVA_COLOR_16BIT565);
	assert(clock_reloads == 1 && tmds_interruptions == 1);

	/* Native has the same 1080p canvas, but a centered x4 capture layout. */
	clear_measurements();
	init_videocap_video_mode(0, 1, ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60, 0);
	printf("RTG -> native, same timing: PLL reloads=%u TMDS interruptions=%u explicit waits=%u us\n",
		clock_reloads, tmds_interruptions, delay_us);
	fflush(stdout);
	assert(clock_reloads == 0 && tmds_interruptions == 0);
	assert(vs.vmode_hsize == 1280 && vs.vmode_vsize == 1024 &&
	       vs.vmode_vdma_rows == 256);
	assert(formatter_ops[MNTVF_OP_VIEWPORT_SIZE_COMMIT] == (1024U << 16 | 1280U));

	clear_measurements();
	init_videocap_video_mode(0, 1, ZZ_VIDEOCAP_OUTPUT_FULL_60,
		VIDEO_VIDEOCAP_SOURCE_SHORT |
		VIDEO_VIDEOCAP_SOURCE_DOUBLED |
		VIDEO_VIDEOCAP_ROWS_CLASS_2);
	assert(vs.vmode_hsize == 1280 && vs.vmode_vsize == 1024 &&
	       vs.vmode_hdiv == 1 && vs.vmode_vdma_rows == 512);
	assert(formatter_ops[MNTVF_OP_SCALE] ==
	       video_formatter_scale_control(3U));

	init_videocap_video_mode(0, 1, ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60, 0);
	/* A native geometry ACK follows the successful VDMA programming, not the
	 * accepted feature write. Every VDMA failure leaves the request pending;
	 * the unchanged serial mismatch retries on the next stable vblank. */
	vs.framebuffer = capture_framebuffer;
	vs.framebuffer_pan_offset = 0x00dff000;
	vs.videocap_enabled_old = 1;
	vs.videocap_video_mode = ZZVMODE_800x600;
	vs.videocap_video_mode_applied = ZZVMODE_800x600;
	vs.videocap_output_profile_requested = ZZ_VIDEOCAP_OUTPUT_FULL_60;
	vs.videocap_output_profile_applied = ZZ_VIDEOCAP_OUTPUT_FULL_60;
	vs.videocap_full_width_applied = 0;
	vs.videocap_ntsc_old = vs.videocap_shres_old = 0;
	vs.videocap_source_class_old = vs.interlace_old = 0;
	vs.vmode_hsize = 800;
	vs.vmode_vsize = 600;
	videocap_detection_reset();
	videocap_zstate = (1U << 21) | (1U << 23);
	assert((video_firmware_capabilities() &
		(ZZ_FW_CAP_VIDEOCAP_GEOMETRY |
		 ZZ_FW_CAP_VIDEOCAP_GEOMETRY_ACK)) == 0U);
	assert(!video_set_videocap_geometry(640, 400));
	videocap_zstate |= MNTZORRO_STATUS_VCAP_VIEWPORT;
	assert(video_set_videocap_geometry(640, 400));
	assert(video_videocap_geometry_value(
		ZZ_CONFIG_KEY_VCAP_GEOMETRY_REQUEST_WIDTH, NULL) == 640);
	assert(video_videocap_geometry_value(
		ZZ_CONFIG_KEY_VCAP_GEOMETRY_STATUS, NULL) ==
		ZZ_VCAP_GEOMETRY_STATUS_PENDING);
	dma_cfg_initialize_status = XST_FAILURE;
	geometry_native_vblank();
	assert_geometry_pending(1);
	dma_cfg_initialize_status = XST_SUCCESS;
	geometry_native_vblank();
	assert(video_videocap_geometry_value(
		ZZ_CONFIG_KEY_VCAP_GEOMETRY_STATUS, NULL) ==
		ZZ_VCAP_GEOMETRY_STATUS_APPLIED_VALID);
	assert(video_videocap_geometry_value(
		ZZ_CONFIG_KEY_VCAP_GEOMETRY_APPLIED_WIDTH, NULL) == 640);
	assert(video_videocap_geometry_value(
		ZZ_CONFIG_KEY_VCAP_GEOMETRY_APPLIED_HEIGHT, NULL) ==
		(uint16_t)vs.vmode_vdma_rows);
	assert(video_videocap_geometry_value(
		ZZ_CONFIG_KEY_VCAP_GEOMETRY_APPLIED_HEIGHT, NULL) != 400);
	assert(dma_setup.HoriSizeInput == 640 * 4);
	assert(dma_setup.Stride == 800 * 4);
	assert(vs.framebuffer_pan_width == 0);
	assert(video_set_videocap_geometry(656, 416));
	dma_config_status = XST_FAILURE;
	geometry_native_vblank();
	assert_geometry_pending(2);
	dma_config_status = XST_SUCCESS;
	geometry_native_vblank();
	assert(video_videocap_geometry_value(
		ZZ_CONFIG_KEY_VCAP_GEOMETRY_APPLIED_SERIAL, NULL) == 2);

	assert(video_set_videocap_geometry(672, 432));
	dma_address_status = XST_FAILURE;
	geometry_native_vblank();
	assert_geometry_pending(3);
	dma_address_status = XST_SUCCESS;
	geometry_native_vblank();
	assert(video_videocap_geometry_value(
		ZZ_CONFIG_KEY_VCAP_GEOMETRY_APPLIED_SERIAL, NULL) == 3);

	assert(video_set_videocap_geometry(688, 448));
	dma_start_status = XST_FAILURE;
	geometry_native_vblank();
	assert_geometry_pending(4);
	dma_start_status = XST_SUCCESS;
	geometry_native_vblank();
	assert(video_videocap_geometry_value(
		ZZ_CONFIG_KEY_VCAP_GEOMETRY_APPLIED_SERIAL, NULL) == 4);

	/* Each axis independently accepts automatic geometry; an invalid request
	 * does not perturb the accepted pair or serial. */
	assert(video_set_videocap_geometry(0, 400));
	assert(video_videocap_geometry_value(
		ZZ_CONFIG_KEY_VCAP_GEOMETRY_REQUEST_WIDTH, NULL) == 0);
	geometry_native_vblank();
	assert(video_videocap_geometry_value(
		ZZ_CONFIG_KEY_VCAP_GEOMETRY_APPLIED_WIDTH, NULL) == 800);
	/* A width override larger than the active mode pitch is clamped
	 * to vs.vmode_hsize to protect VDMA stride integrity. */
	assert(video_set_videocap_geometry(1280, 400));
	geometry_native_vblank();
	assert(video_videocap_geometry_value(
		ZZ_CONFIG_KEY_VCAP_GEOMETRY_APPLIED_WIDTH, NULL) == 800);
	assert(dma_setup.HoriSizeInput == 800 * 4);
	assert(dma_setup.Stride == 800 * 4);

	assert(!video_set_videocap_geometry(255, 400));
	assert(video_videocap_geometry_value(
		ZZ_CONFIG_KEY_VCAP_GEOMETRY_REQUEST_SERIAL, NULL) == 6);
	assert(video_videocap_geometry_value(
		ZZ_CONFIG_KEY_VCAP_GEOMETRY_STATUS, NULL) ==
		(ZZ_VCAP_GEOMETRY_STATUS_APPLIED_VALID |
		 ZZ_VCAP_GEOMETRY_STATUS_REJECTED));
	assert((video_firmware_capabilities() &
		(ZZ_FW_CAP_VIDEOCAP_GEOMETRY |
		 ZZ_FW_CAP_VIDEOCAP_GEOMETRY_ACK |
		 ZZ_FW_CAP_VIDEOCAP_STATS)) ==
		(ZZ_FW_CAP_VIDEOCAP_GEOMETRY |
		 ZZ_FW_CAP_VIDEOCAP_GEOMETRY_ACK |
		 ZZ_FW_CAP_VIDEOCAP_STATS));
	/* An accepted request invalidates the ACK even when the 16-bit
	 * serial wraps onto the applied value. */
	vs.videocap_geometry_applied_serial = 0xffff;
	vs.videocap_geometry_request_serial = 0xffff;
	assert(video_set_videocap_geometry(640, 400));
	assert(vs.videocap_geometry_request_serial == 0);
	assert(!vs.videocap_geometry_applied_valid);
	assert(video_videocap_geometry_value(
		ZZ_CONFIG_KEY_VCAP_GEOMETRY_STATUS, NULL) &
		ZZ_VCAP_GEOMETRY_STATUS_PENDING);
	/* The viewport and VDMA must agree on the active filtered canvas,
	 * including restoring Automatic without an output-mode transition. */
	assert(video_set_videocap_geometry(640, 240));
	geometry_native_vblank();
	assert(formatter_ops[MNTVF_OP_VIEWPORT_POS] == (60U << 16 | 80U));
	assert(formatter_ops[MNTVF_OP_VIEWPORT_SIZE_COMMIT] == (480U << 16 | 640U));
	assert(dma_setup.HoriSizeInput == 640 * 4 &&
	       dma_setup.Stride == 800 * 4 && dma_setup.VertSizeInput == 240);
	assert(video_set_videocap_geometry(0, 0));
	geometry_native_vblank();
	assert(formatter_ops[MNTVF_OP_VIEWPORT_POS] == 0);
	assert(formatter_ops[MNTVF_OP_VIEWPORT_SIZE_COMMIT] == (600U << 16 | 800U));
	/* 15 kHz filtered 800x600 keeps the tuned origin. A short-line PAL
	 * field on that same configured base is shown on 720x576 and must
	 * start at the capture row, not 190 words before it. */
	assert(vs.framebuffer_pan_offset == VIDEO_VDMA_CAPTURE_PAN_PAL_800X600);
	videocap_zstate |= 1U << 11;
	assert(video_set_videocap_geometry(0, 0));
	geometry_native_vblank();
	assert(vs.framebuffer_pan_offset == VIDEO_VDMA_CAPTURE_PAN_BASE);
	assert(dma_setup.Stride == 720 * 4);
	videocap_zstate &= ~(1U << 11);
	vs.videocap_source_class_old = 0;

	videocap_zstate |= 1U << 22;
	assert(video_set_videocap_geometry(640, 200));
	geometry_native_vblank();
	assert(formatter_ops[MNTVF_OP_VIEWPORT_POS] == (40U << 16 | 40U));
	assert(formatter_ops[MNTVF_OP_VIEWPORT_SIZE_COMMIT] == (400U << 16 | 640U));
	assert(dma_setup.Stride == 720 * 4 && dma_setup.VertSizeInput == 200);

	/* Euro72 / DblNTSC class-2 NTSC keeps all 512 scanout rows at x2,
	 * not the 400 rows consumed by a legacy 800-line letterbox. */
	vs.videocap_output_profile_requested = ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60;
	videocap_zstate |= MNTZORRO_STATUS_VCAP_FULLRATE |
	                  MNTZORRO_STATUS_VCAP_VIEWPORT |
	                  (1U << 11) | (1U << 12) | (2U << 8);
	assert(video_set_videocap_geometry(0, 0));
	geometry_native_vblank();
	assert(formatter_ops[MNTVF_OP_VIEWPORT_POS] == (28U << 16 | 320U));
	assert(formatter_ops[MNTVF_OP_VIEWPORT_SIZE_COMMIT] == (1024U << 16 | 1280U));
	assert(dma_setup.VertSizeInput == 512);
	/* Same class, smaller published field: DMA and viewport follow the
	 * count, or the bucket tail keeps the previous source. */
	videocap_live_geometry_reg = videocap_live_geometry(1280U, 427U);
	isr_video(NULL);
	assert(dma_setup.VertSizeInput == 427);
	assert(formatter_ops[MNTVF_OP_VIEWPORT_POS] == (113U << 16 | 320U));
	assert(formatter_ops[MNTVF_OP_VIEWPORT_SIZE_COMMIT] ==
	       (854U << 16 | 1280U));
	videocap_live_geometry_reg = 0;
	/* DblPAL fields alternate 287/288. Both clamp to the class-2
	 * bucket, so the second field must not restart VDMA. */
	videocap_zstate |= 1U << 24;
	videocap_live_geometry_reg = videocap_live_geometry(1280U, 287U);
	geometry_native_vblank();
	assert(dma_setup.VertSizeInput == 512);
	clear_measurements();
	videocap_live_geometry_reg = videocap_live_geometry(1280U, 288U);
	isr_video(NULL);
	assert(dma_starts == 0);
	assert(dma_setup.VertSizeInput == 512);
	/* Euro72 fields alternate 213/214: post-crop published counts
	 * 195/196 double to 390/392, both below the class-2 bucket, so the
	 * delta must not restart VDMA either. */
	videocap_live_geometry_reg = videocap_live_geometry(1280U, 195U);
	geometry_native_vblank();
	assert(dma_setup.VertSizeInput == 390);
	clear_measurements();
	videocap_live_geometry_reg = videocap_live_geometry(1280U, 196U);
	isr_video(NULL);
	assert(dma_starts == 0);
	assert(dma_setup.VertSizeInput == 390);
	videocap_zstate &= ~(1U << 24);
	videocap_live_geometry_reg = 0;
	/* Super72 completes its lines before the 1280-word pitch: the
	 * measured width bounds the fetch and the shown viewport, and an
	 * unchanged width must not restart VDMA. REG1 holds a pending host
	 * payload, so capture geometry must come only from the packed REG2 read. */
	videocap_zstate &= ~(1U << 12);
	zorro_ram_write_data = 0x00ca0000U;
	videocap_live_geometry_reg = videocap_live_geometry(1008U, 0U);
	geometry_native_vblank();
	assert(dma_setup.HoriSizeInput == 1008 * 4);
	assert(dma_setup.VertSizeInput == 512);
	assert(formatter_ops[MNTVF_OP_VIEWPORT_POS] == (28U << 16 | 456U));
	assert(formatter_ops[MNTVF_OP_VIEWPORT_SIZE_COMMIT] ==
	       (1024U << 16 | 1008U));
	clear_measurements();
	isr_video(NULL);
	assert(dma_starts == 0);
	videocap_live_geometry_reg = 0;
	videocap_zstate |= 1U << 12;
	printf("Native geometry: PAL 640x480@(80,60), NTSC 640x400@(40,40), "
	       "Automatic restored, short NTSC retains 512 rows\n");
	/* Restore the pre-regression centered native state for the independent
	 * mode-switch transaction cases below. */
	vs.videocap_width_override = 0;
	vs.videocap_height_override = 0;
	init_videocap_video_mode(0, 1, ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60, 0);

	/* DPMS is formatter-owned: the fast path must still restore syncs. */
	video_set_dpms(ZZ_DPMS_OFF);
	assert(formatter_ops[MNTVF_OP_DPMS] == ZZ_DPMS_OFF);
	clear_measurements();
	video_mode_init(ZZVMODE_1920x1080_60, 0, MNTVA_COLOR_16BIT565);
	assert(clock_reloads == 0 && tmds_interruptions == 0);
	assert(dma_starts == 1 && dma_setup.VertSizeInput == 1080 &&
	       dma_setup.HoriSizeInput == 1920 * 2);
	assert(formatter_ops[MNTVF_OP_COLORMODE] == MNTVA_COLOR_16BIT565);
	assert(formatter_ops[MNTVF_OP_DPMS] == ZZ_DPMS_ON);

	/* An identical custom mode is also safe, regardless of its mode ID. */
	preset_video_modes[ZZVMODE_CUSTOM] = preset_video_modes[ZZVMODE_1920x1080_60];
	clear_measurements();
	video_mode_init(ZZVMODE_CUSTOM, 0, MNTVA_COLOR_32BIT);
	assert(clock_reloads == 0 && tmds_interruptions == 0);

	/* Editing a live custom slot must not mutate the applied snapshot. */
	++preset_video_modes[ZZVMODE_CUSTOM].hstart;
	clear_measurements();
	video_mode_init(ZZVMODE_CUSTOM, 0, MNTVA_COLOR_32BIT);
	assert(clock_reloads == 0 && tmds_interruptions == 1);

	/* 1080p50 has the same PLL as 1080p60, but different line timing. */
	clear_measurements();
	video_mode_init(ZZVMODE_1920x1080_50, 0, MNTVA_COLOR_32BIT);
	assert(clock_reloads == 0 && tmds_interruptions == 1);
	assert(formatter_ops[MNTVF_OP_MAX] == (1125U << 16 | 2640U));

	/* A failed/lost PLL must not be hidden by a matching software cache. */
	clock_locked = 0;
	clear_measurements();
	video_mode_init(ZZVMODE_1920x1080_50, 0, MNTVA_COLOR_32BIT);
	assert(clock_reloads == 1 && tmds_interruptions == 1);
	clock_load = CLK_WIZ_RECONFIG_LOAD;
	clear_measurements();
	video_mode_init(ZZVMODE_1920x1080_50, 0, MNTVA_COLOR_32BIT);
	assert(clock_reloads == 1 && tmds_interruptions == 1);

	clear_measurements();
	video_mode_init(ZZVMODE_800x600, 2, MNTVA_COLOR_32BIT);
	assert(clock_reloads == 1 && tmds_interruptions == 1);

	/* ------- staged custom modeline transaction (issue97) ------- */
	struct zz_video_mode slot_saved;
	uint32_t max_saved, dims_saved, hs_saved;

	/* A commit without SELECT is rejected and leaves hardware untouched. */
	clear_measurements();
	max_saved = formatter_ops[MNTVF_OP_MAX];
	dims_saved = formatter_ops[MNTVF_OP_DIMENSIONS];
	hs_saved = formatter_ops[MNTVF_OP_HS];
	assert(commit_custom(MNTVA_COLOR_16BIT565) ==
	       ZZ_CUSTOM_STATUS_INVALID);
	assert(video_custom_status() == ZZ_CUSTOM_STATUS_INVALID);
	assert(clock_reloads == 0 && tmds_interruptions == 0 &&
	       dma_starts == 0);
	assert(vs.video_mode == ZZVMODE_800x600);
	assert(formatter_ops[MNTVF_OP_MAX] == max_saved &&
	       formatter_ops[MNTVF_OP_DIMENSIONS] == dims_saved &&
	       formatter_ops[MNTVF_OP_HS] == hs_saved);

	/* An incomplete request (one field missing) stays INVALID. */
	clear_measurements();
	stage_custom_960(1024, ZZ_CUSTOM_VTOTAL);
	assert(commit_custom(MNTVA_COLOR_16BIT565) ==
	       ZZ_CUSTOM_STATUS_INVALID);
	assert(clock_reloads == 0 && tmds_interruptions == 0 &&
	       dma_starts == 0);
	assert(vs.video_mode == ZZVMODE_800x600);

	/* Malformed geometry (hsync inside the active area) is rejected. */
	clear_measurements();
	stage_custom_960(100, UINT16_MAX);
	assert(commit_custom(MNTVA_COLOR_16BIT565) ==
	       ZZ_CUSTOM_STATUS_INVALID);
	assert(clock_reloads == 0 && tmds_interruptions == 0 &&
	       dma_starts == 0);

	/* Mixed H/V polarity has no hardware representation: rejected. */
	clear_measurements();
	stage_custom_960(1024, ZZ_CUSTOM_POLARITY);
	video_custom_set_param(ZZ_CUSTOM_POLARITY);
	video_custom_set_value(3);
	assert(commit_custom(MNTVA_COLOR_16BIT565) ==
	       ZZ_CUSTOM_STATUS_INVALID);
	assert(clock_reloads == 0 && tmds_interruptions == 0 &&
	       dma_starts == 0);

	/* An unknown param id poisons the transaction until a new SELECT. */
	stage_custom_960(1024, UINT16_MAX);
	video_custom_set_param(9); /* MHZ-style word: not a protocol field */
	video_custom_set_value(999);
	assert(commit_custom(MNTVA_COLOR_16BIT565) ==
	       ZZ_CUSTOM_STATUS_INVALID);
	/* VALUE writes after the poison must not heal or apply anything. */
	video_custom_set_param(ZZ_CUSTOM_HTOTAL);
	video_custom_set_value(1188);
	assert(commit_custom(MNTVA_COLOR_16BIT565) ==
	       ZZ_CUSTOM_STATUS_INVALID);
	assert(vs.video_mode == ZZVMODE_800x600);

	/* Wrong slot and scale bits in the commit word are rejected, without
	 * consuming the staged transaction. */
	stage_custom_960(1024, UINT16_MAX);
	assert(video_custom_commit(19U | (MNTVA_COLOR_16BIT565 << 8)) ==
	       ZZ_CUSTOM_STATUS_INVALID);
	assert(video_custom_commit(ZZ_CUSTOM_MODE_SLOT | 0x1000U) ==
	       ZZ_CUSTOM_STATUS_INVALID);

	/* Valid 960x720 commit: timing, clock, DMA and metadata all applied. */
	clear_measurements();
	assert(commit_custom(MNTVA_COLOR_16BIT565) == ZZ_CUSTOM_STATUS_OK);
	assert(video_custom_status() == ZZ_CUSTOM_STATUS_OK);
	assert(vs.video_mode == ZZVMODE_CUSTOM &&
	       vs.colormode == MNTVA_COLOR_16BIT565 && vs.scalemode == 0);
	assert(vs.vmode_hsize == 960 && vs.vmode_vsize == 720 &&
	       vs.vmode_hdiv == 2 && vs.vmode_vdma_rows == 720);
	assert(formatter_ops[MNTVF_OP_MAX] == (746U << 16 | 1188U));
	assert(formatter_ops[MNTVF_OP_DIMENSIONS] == (720U << 16 | 960U));
	assert(formatter_ops[MNTVF_OP_HS] == (1024U << 16 | 1040U));
	assert(formatter_ops[MNTVF_OP_VS] == (729U << 16 | 733U));
	assert(formatter_ops[MNTVF_OP_POLARITY] == 0);
	assert(formatter_ops[MNTVF_OP_COLORMODE] == MNTVA_COLOR_16BIT565);
	assert(formatter_ops[MNTVF_OP_SCALE] ==
	       video_formatter_scale_control(0));
	assert(clock_reloads == 1 && tmds_interruptions == 1 && dma_starts == 1);
	assert(dma_setup.VertSizeInput == 720);
	assert(dma_setup.HoriSizeInput == 960 * 4 / 2); /* 16-bit color */
	assert(clock_mul_div == (53U << 8 | 4U) && clock_div2 == 25U);
	slot_saved = preset_video_modes[ZZVMODE_CUSTOM];
	assert(slot_saved.phz == 53000000 && slot_saved.vhz == 60 &&
	       slot_saved.mhz == 53 && slot_saved.hdmi == 0);

	/* Re-commit of the identical modeline keeps the fast same-timing path. */
	clear_measurements();
	stage_custom_960(1024, UINT16_MAX);
	assert(commit_custom(MNTVA_COLOR_16BIT565) == ZZ_CUSTOM_STATUS_OK);
	assert(clock_reloads == 0 && tmds_interruptions == 0 && dma_starts == 1);

	/* Staging words alone never mutate the applied slot or the output. */
	clear_measurements();
	stage_custom_960(1030, ZZ_CUSTOM_HSTART);
	assert(preset_video_modes[ZZVMODE_CUSTOM].hstart == 1024);
	assert(vs.video_mode == ZZVMODE_CUSTOM);
	assert(clock_reloads == 0 && tmds_interruptions == 0 &&
	       dma_starts == 0);
	/* Completing and committing the changed sync does take effect. */
	video_custom_set_param(ZZ_CUSTOM_HSTART);
	video_custom_set_value(1030);
	assert(commit_custom(MNTVA_COLOR_16BIT565) == ZZ_CUSTOM_STATUS_OK);
	assert(formatter_ops[MNTVF_OP_HS] == (1030U << 16 | 1040U));
	assert(clock_reloads == 0 && tmds_interruptions == 1);

	/* custom -> native -> custom round trip. */
	init_videocap_video_mode(0, 1, ZZ_VIDEOCAP_OUTPUT_FULL_60, 0);
	assert(vs.video_mode == ZZVMODE_1280x1024_NATIVE_60);
	assert(formatter_ops[MNTVF_OP_MAX] == (1066U << 16 | 1688U));
	clear_measurements();
	stage_custom_960(1024, UINT16_MAX);
	assert(commit_custom(MNTVA_COLOR_32BIT) == ZZ_CUSTOM_STATUS_OK);
	assert(vs.video_mode == ZZVMODE_CUSTOM);
	assert(dma_setup.VertSizeInput == 720);
	assert(dma_setup.HoriSizeInput == 960 * 4); /* 32-bit color */
	assert(clock_reloads == 1 && tmds_interruptions == 1);
	assert(clock_mul_div == (53U << 8 | 4U) && clock_div2 == 25U);

	/* Native output is the rollback target for a failed custom lock. */
	init_videocap_video_mode(0, 1, ZZ_VIDEOCAP_OUTPUT_FULL_60, 0);
	assert(vs.video_mode == ZZVMODE_1280x1024_NATIVE_60);
	/* Model a detected progressive NTSC field: rollback must retain both
	 * its 200-row VDMA contract and the formatter's fractional scale word. */
	vs.scalemode = (int)video_videocap_scalemode(1, 0, 0);
	vs.vmode_vdma_rows =
		video_videocap_source_rows(vs.vmode_vsize, 1, 1, 0, 0, 0, 0);
	vs.interlace_old = 0;
	video_formatter_write(video_videocap_scale_control(1, 1, 0, 0),
	                      MNTVF_OP_SCALE);
	init_vdma(vs.vmode_hsize, vs.vmode_vdma_rows, 1, 0);
	/* A native letterbox written after mode init must be the rollback
	 * target, not the viewport mode init cached. */
	video_formatter_write((112U << 16) | 0U, MNTVF_OP_VIEWPORT_POS);
	video_formatter_write((800U << 16) | 1280U, MNTVF_OP_VIEWPORT_SIZE_COMMIT);
	slot_saved = preset_video_modes[ZZVMODE_CUSTOM];

	/* PLL lock failure: CLOCK_FAILED, old output replayed exactly. */
	video_set_dpms(ZZ_DPMS_OFF);
	clear_measurements();
	clock_fail_locks = 1;
	assert(commit_custom(MNTVA_COLOR_32BIT) ==
	       ZZ_CUSTOM_STATUS_CLOCK_FAILED);
	assert(video_custom_status() == ZZ_CUSTOM_STATUS_CLOCK_FAILED);
	assert(clock_reloads == 2); /* failed attempt + restore */
	assert(tmds_interruptions == 2); /* prepare(new) + prepare(old) */
	assert(dma_starts == 0); /* VDMA never left the old geometry */
	assert(formatter_ops[MNTVF_OP_MAX] == (1066U << 16 | 1688U));
	assert(formatter_ops[MNTVF_OP_DIMENSIONS] == (1024U << 16 | 1280U));
	assert(formatter_ops[MNTVF_OP_HS] == (1328U << 16 | 1440U));
	assert(formatter_ops[MNTVF_OP_SCALE] ==
	       video_videocap_scale_control(1, 1, 0, 0));
	assert(formatter_ops[MNTVF_OP_VIEWPORT_POS] == (112U << 16));
	assert(formatter_ops[MNTVF_OP_VIEWPORT_SIZE_COMMIT] ==
	       (800U << 16 | 1280U));
	assert(delay_us > CLK_WIZ_LOCK_TIMEOUT_US / 2U); /* bounded poll ran */
	assert(memcmp(&preset_video_modes[ZZVMODE_CUSTOM], &slot_saved,
	       sizeof(slot_saved)) == 0);
	assert(vs.card_feature_enabled[CARD_FEATURE_DPMS] == ZZ_DPMS_OFF);
	assert(formatter_ops[MNTVF_OP_DPMS] == ZZ_DPMS_OFF);

	/* Successful retry without re-staging: the failed commit kept the
	 * staged words, so one commit word is enough. */
	clear_measurements();
	assert(commit_custom(MNTVA_COLOR_32BIT) == ZZ_CUSTOM_STATUS_OK);
	assert(vs.video_mode == ZZVMODE_CUSTOM &&
	       vs.colormode == MNTVA_COLOR_32BIT && vs.scalemode == 0);
	assert(dma_setup.VertSizeInput == 720);
	assert(dma_setup.HoriSizeInput == 960 * 4);
	assert(formatter_ops[MNTVF_OP_COLORMODE] == MNTVA_COLOR_32BIT);
	assert(formatter_ops[MNTVF_OP_SCALE] ==
	       video_formatter_scale_control(0));
	assert(clock_reloads == 1 && tmds_interruptions == 1);
	assert(clock_mul_div == (53U << 8 | 4U) && clock_div2 == 25U);
	assert(vs.card_feature_enabled[CARD_FEATURE_DPMS] == ZZ_DPMS_ON);
	assert(formatter_ops[MNTVF_OP_DPMS] == ZZ_DPMS_ON);

	/* An IRQ must not replace a failed requested PLL with a locked native
	 * clock, or mutate centered viewport state underneath rollback. */
	init_videocap_video_mode(0, 1,
		ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_MATCH, 0);
	uint32_t saved_formatter[32];
	memcpy(saved_formatter, formatter_ops, sizeof(saved_formatter));
	XAxiVdma_DmaSetup saved_dma = dma_setup;
	clear_measurements();
	clock_fail_locks = 1;
	inject_native_irq = 1;
	assert(commit_custom(MNTVA_COLOR_16BIT565) ==
	       ZZ_CUSTOM_STATUS_CLOCK_FAILED);
	assert(deferred_native_irqs == 1 && video_irq_enabled == 1);
	assert(formatter_ops[MNTVF_OP_DIMENSIONS] ==
	       saved_formatter[MNTVF_OP_DIMENSIONS]);
	assert(formatter_ops[MNTVF_OP_VIEWPORT_POS] ==
	       saved_formatter[MNTVF_OP_VIEWPORT_POS]);
	assert(formatter_ops[MNTVF_OP_VIEWPORT_SIZE_COMMIT] ==
	       saved_formatter[MNTVF_OP_VIEWPORT_SIZE_COMMIT]);
	assert(formatter_ops[MNTVF_OP_SOURCE_SYNC] ==
	       saved_formatter[MNTVF_OP_SOURCE_SYNC]);
	assert(memcmp(&dma_setup, &saved_dma, sizeof(saved_dma)) == 0);
	/* Once capture has enabled source locking, rollback preserves it too.
	 * Capture may have changed from progressive to woven NTSC after init. */
	vs.scalemode = (int)video_videocap_scalemode(1, 1, 0);
	vs.vmode_vdma_rows =
		video_videocap_source_rows(vs.vmode_vsize, 1, 1, 1, 0, 0, 0);
	vs.interlace_old = 1;
	video_formatter_write(video_videocap_scale_control(1, 1, 1, 0),
	                      MNTVF_OP_SCALE);
	init_vdma(vs.vmode_hsize, vs.vmode_vdma_rows, 1, 0);
	saved_dma = dma_setup;
	video_formatter_write(1, MNTVF_OP_SOURCE_SYNC);
	clock_fail_locks = 1;
	assert(commit_custom(MNTVA_COLOR_16BIT565) ==
	       ZZ_CUSTOM_STATUS_CLOCK_FAILED);
	assert(formatter_ops[MNTVF_OP_SOURCE_SYNC] == 1);
	assert(formatter_ops[MNTVF_OP_SCALE] ==
	       video_videocap_scale_control(1, 1, 1, 0));
	assert(vs.interlace_old == 1);
	assert(memcmp(&dma_setup, &saved_dma, sizeof(saved_dma)) == 0);

	/* A caller that already disabled video IRQs retains that state. */
	video_irq_enabled = 0;
	assert(commit_custom(MNTVA_COLOR_16BIT565) == ZZ_CUSTOM_STATUS_OK);
	assert(video_irq_enabled == 0);
	video_irq_enabled = 1;

	/* A new SELECT resets the reported status to IDLE. */
	video_custom_select(ZZ_CUSTOM_MODE_SLOT);
	assert(video_custom_status() == ZZ_CUSTOM_STATUS_IDLE);

	/* Ordinary RTG never installed a viewport. A failed custom lock must
	 * not write a stale native rectangle over the restored canvas. */
	video_mode_init(ZZVMODE_1920x1080_60, 0, MNTVA_COLOR_16BIT565);
	formatter_ops[MNTVF_OP_VIEWPORT_POS] = 0xA5A5A5A5U;
	formatter_ops[MNTVF_OP_VIEWPORT_SIZE_COMMIT] = 0xA5A5A5A5U;
	stage_custom_960(1024, UINT16_MAX);
	clock_fail_locks = 1;
	assert(commit_custom(MNTVA_COLOR_32BIT) ==
	       ZZ_CUSTOM_STATUS_CLOCK_FAILED);
	assert(formatter_ops[MNTVF_OP_VIEWPORT_POS] == 0xA5A5A5A5U);
	assert(formatter_ops[MNTVF_OP_VIEWPORT_SIZE_COMMIT] == 0xA5A5A5A5U);
	assert(formatter_ops[MNTVF_OP_DIMENSIONS] ==
	       (((uint32_t)preset_video_modes[ZZVMODE_1920x1080_60].vres << 16) |
	        (uint32_t)preset_video_modes[ZZVMODE_1920x1080_60].hres));
	puts("video mode switch: PASS");
	return 0;
}
