/*
 * MNT ZZ9000 Amiga Graphics and Coprocessor Card Operating System (ZZ9000OS)
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Cortex-A9 private watchdog. Armed once at boot, kicked from the main
 * loop; if the firmware wedges for ~12.9 s the whole PS resets and the
 * card reboots from BOOT.bin.
 */

#include <stdio.h>
#include "xparameters.h"
#include "xscuwdt.h"
#include "xil_io.h"
#include "watchdog.h"

#define SLCR_UNLOCK_ADDR    0xF8000008U
#define SLCR_LOCK_ADDR      0xF8000004U
#define SLCR_UNLOCK_CODE    0x0000DF0DU
#define SLCR_LOCK_CODE      0x0000767BU
#define SLCR_RS_AWDT_CTRL   0xF800024CU

static XScuWdt wdt;
static int wdt_ready = 0;

int watchdog_init(void)
{
	XScuWdt_Config *cfg = XScuWdt_LookupConfig(XPAR_SCUWDT_0_DEVICE_ID);
	if (!cfg)
		return XST_FAILURE;
	if (XScuWdt_CfgInitialize(&wdt, cfg, cfg->BaseAddr) != XST_SUCCESS)
		return XST_FAILURE;

	/* RS_AWDT_CTRL bit 0: 0 = AWDT0 expiry resets the whole PS (the
	 * power-on default), 1 = CPU-only reset. Force the system reset in
	 * case a bootloader changed it. */
	Xil_Out32(SLCR_UNLOCK_ADDR, SLCR_UNLOCK_CODE);
	Xil_Out32(SLCR_RS_AWDT_CTRL, Xil_In32(SLCR_RS_AWDT_CTRL) & ~1U);
	Xil_Out32(SLCR_LOCK_ADDR, SLCR_LOCK_CODE);

	/* The private watchdog clocks at CPU_3x2x (~333 MHz), so the maximum
	 * load value gives ~12.9 s — far above any legitimate main loop stall
	 * (USB proxy caps at 1 s, SD/FatFs chunks run per loop iteration). */
	XScuWdt_LoadWdt(&wdt, 0xFFFFFFFFU);
	XScuWdt_SetWdMode(&wdt);
	XScuWdt_Start(&wdt);
	wdt_ready = 1;
	printf("[wdt] private watchdog armed (~12.9 s).\n");
	return XST_SUCCESS;
}

void watchdog_kick(void)
{
	if (wdt_ready)
		XScuWdt_RestartWdt(&wdt);
}
