/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>

#include "video.h"

static int expect_u32(const char *label, uint32_t actual, uint32_t expected)
{
	if (actual != expected) {
		printf("%s: got 0x%08lx expected 0x%08lx\n", label,
		       (unsigned long)actual, (unsigned long)expected);
		return 0;
	}

	return 1;
}

int main(void)
{
	uint32_t data;

	data = videocap_control_pack(0U, 1U,
	                             VIDEOCAP_CROP_H_COMPAT,
	                             VIDEOCAP_CROP_V_COMPAT,
	                             0U, 0U);
	if (!expect_u32("both axes automatic", data,
	                VIDEOCAP_CROP_H_AUTO_FLAG |
	                VIDEOCAP_CROP_V_AUTO_FLAG |
	                (VIDEOCAP_CROP_V_COMPAT << 16) |
	                (VIDEOCAP_CROP_H_COMPAT << 4) | (1U << 2)))
		return 1;

	data = videocap_control_pack(2U, 0U, 279U, 41U, 1U, 0U);
	if (!expect_u32("vertical automatic only", data,
	                VIDEOCAP_CROP_V_AUTO_FLAG |
	                (VIDEOCAP_CROP_V_COMPAT << 16) |
	                (279U << 4) | 2U))
		return 2;

	data = videocap_control_pack(3U, 0U, 280U, 42U, 0U, 1U);
	if (!expect_u32("horizontal automatic only", data,
	                VIDEOCAP_CROP_H_AUTO_FLAG |
	                (42U << 16) |
	                (VIDEOCAP_CROP_H_COMPAT << 4) | 3U))
		return 3;

	data = videocap_control_pack(1U, 1U, 0U, 4095U, 1U, 1U);
	if (!expect_u32("literal boundary values", data,
	                (4095U << 16) | (1U << 2) | 1U))
		return 4;

	data = videocap_control_pack(3U, 3U, 8191U, 8191U, 1U, 1U);
	if (!expect_u32("fields remain bounded", data,
	                (4095U << 16) | (4095U << 4) | (1U << 2) | 3U))
		return 5;

	return 0;
}
