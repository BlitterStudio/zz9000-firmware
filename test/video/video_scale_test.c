/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "video_scale.h"
#include <stdint.h>
#include <stdio.h>

static int expect_u32(const char *label, uint32_t actual, uint32_t expected)
{
	if (actual != expected) {
		printf("%s: got %u expected %u\n", label, actual, expected);
		return 0;
	}

	return 1;
}

int main(void)
{
	if (!expect_u32("vertical scale x1",
	                video_vertical_scale_factor(0U), 1U))
		return 1;
	if (!expect_u32("vertical scale x2",
	                video_vertical_scale_factor(2U), 2U))
		return 2;
	if (!expect_u32("vertical scale x4",
	                video_vertical_scale_factor(4U), 4U))
		return 3;
	if (!expect_u32("x2 keeps legacy sprite doubling",
	                video_formatter_scale_control(2U), 10U))
		return 4;
	if (!expect_u32("x4 leaves sprite doubling independent",
	                video_formatter_scale_control(4U), 4U))
		return 5;

	return 0;
}
