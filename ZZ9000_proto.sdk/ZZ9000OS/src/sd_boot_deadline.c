/*
 * Bounded early-boot SD access: deadline state (plan fast-ram-cfg U3)
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The hit() check itself lives next to each vendor loop (xsdps.c,
 * diskio.c) as a small static helper so the vendor diffs stay
 * self-contained; this file owns only the arming state.
 */

#include <stdint.h>
#include "xtime_l.h"
#include "sd_boot_deadline.h"

volatile uint64_t sd_boot_deadline_xtime;
volatile uint8_t sd_boot_deadline_fired;

void sd_boot_deadline_arm(uint32_t ms)
{
	XTime now;

	XTime_GetTime(&now);
	sd_boot_deadline_xtime =
		now + ((uint64_t)ms * COUNTS_PER_SECOND) / 1000U;
	sd_boot_deadline_fired = 0;
}

void sd_boot_deadline_disarm(void)
{
	sd_boot_deadline_xtime = 0;
}
