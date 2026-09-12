/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Run the production mode transaction against simulated MMIO/I2C peripherals.
 * A same-timing RTG/native switch must not remove the monitor's signal; real
 * timing changes and a lost PLL lock must still take the retraining path.
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
static uint8_t transmitter[256], i2c_register;
static uint32_t formatter_data, formatter_ops[32];
static XAxiVdma_DmaSetup dma_setup;
static unsigned dma_starts;

void test_xil_out32(uintptr_t address, uint32_t value)
{
	if (address == XPAR_CLK_WIZ_0_BASEADDR + CLK_WIZ_RECONFIG_OFFSET) {
		++clock_reloads;
		clock_locked = 1;
		clock_load = 0;
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
void usleep(unsigned long useconds) { delay_us += useconds; }

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
	puts("video mode switch: PASS");
	return 0;
}
