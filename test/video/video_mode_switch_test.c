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
	return 0;
}

uint32_t smp_local_irq_save(void) { return 0; }
void smp_local_irq_restore(uint32_t state) { (void)state; }
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
				ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60);
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
	return XST_SUCCESS;
}
int XAxiVdma_DmaConfig(XAxiVdma *instance, u16 direction,
		XAxiVdma_DmaSetup *config)
{
	(void)instance; (void)direction;
	dma_setup = *config;
	return XST_SUCCESS;
}
int XAxiVdma_DmaSetBufferAddr(XAxiVdma *instance, u16 direction,
		UINTPTR *addresses)
{
	(void)instance; (void)direction; (void)addresses;
	return XST_SUCCESS;
}
int XAxiVdma_DmaStart(XAxiVdma *instance, u16 direction)
{
	(void)instance; (void)direction;
	++dma_starts;
	return XST_SUCCESS;
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
}

int main(void)
{
	/* Cold start must initialize the physical output. */
	video_mode_init(ZZVMODE_1920x1080_60, 0, MNTVA_COLOR_16BIT565);
	assert(clock_reloads == 1 && tmds_interruptions == 1);

	/* Native has the same 1080p canvas, but a centered x4 capture layout. */
	clear_measurements();
	init_videocap_video_mode(0, 1, ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60);
	printf("RTG -> native, same timing: PLL reloads=%u TMDS interruptions=%u explicit waits=%u us\n",
		clock_reloads, tmds_interruptions, delay_us);
	fflush(stdout);
	assert(clock_reloads == 0 && tmds_interruptions == 0);
	assert(vs.vmode_hsize == 1280 && vs.vmode_vsize == 1024 && vs.vmode_vdiv == 4);
	assert(formatter_ops[MNTVF_OP_VIEWPORT_SIZE_COMMIT] == (1024U << 16 | 1280U));

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
	       vs.vmode_hdiv == 2 && vs.vmode_vdiv == 1);
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
	init_videocap_video_mode(0, 1, ZZ_VIDEOCAP_OUTPUT_FULL_60);
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
	init_videocap_video_mode(0, 1, ZZ_VIDEOCAP_OUTPUT_FULL_60);
	assert(vs.video_mode == ZZVMODE_1280x1024_NATIVE_60);
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
		ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_MATCH);
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
	/* Once capture has enabled source locking, rollback preserves it too. */
	/* Capture may have changed from x4 to interlaced x2 after mode init. */
	vs.scalemode = (int)video_videocap_scalemode(1, 1);
	vs.vmode_vdiv = (int)video_vertical_scale_factor((uint32_t)vs.scalemode);
	vs.interlace_old = 1;
	video_formatter_write(video_formatter_scale_control((uint32_t)vs.scalemode),
	                      MNTVF_OP_SCALE);
	init_vdma(vs.vmode_hsize, vs.vmode_vsize, 1, vs.vmode_vdiv, 0);
	saved_dma = dma_setup;
	video_formatter_write(1, MNTVF_OP_SOURCE_SYNC);
	clock_fail_locks = 1;
	assert(commit_custom(MNTVA_COLOR_16BIT565) ==
	       ZZ_CUSTOM_STATUS_CLOCK_FAILED);
	assert(formatter_ops[MNTVF_OP_SOURCE_SYNC] == 1);
	assert(formatter_ops[MNTVF_OP_SCALE] ==
	       video_formatter_scale_control((uint32_t)vs.scalemode));
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

	puts("video mode switch: PASS");
	return 0;
}
