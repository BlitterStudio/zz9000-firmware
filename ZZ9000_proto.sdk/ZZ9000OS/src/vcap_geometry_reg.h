/*
 * Capture-window geometry over its own write-only registers.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * REG_ZZ_VCAP_GEOMETRY_WIDTH stages the width, then
 * REG_ZZ_VCAP_GEOMETRY_COMMIT carries the height and applies the pair.
 * Neither register is a blitter operand, so the RTG driver's cached
 * REG_ZZ_USER1/USER2 values stay valid. The 2.9.0 path (width in
 * REG_ZZ_USER2, CARD_FEATURE_VIDEOCAP_GEOMETRY in REG_ZZ_USER1, height
 * through REG_ZZ_SET_FEATURE) still works for older clients.
 */
#ifndef VCAP_GEOMETRY_REG_H
#define VCAP_GEOMETRY_REG_H

#include <stdint.h>

/* Value of ZZ_CONFIG_KEY_VCAP_GEOMETRY_REGS. Firmware without the
 * registers reports the key as absent, so clients use the presence flag
 * to choose between this path and the 2.9.0 feature sequence. */
#define VCAP_GEOMETRY_REGS_ABI 1U

struct vcap_geometry_reg {
	uint16_t width;
	uint8_t staged;
};

static inline void vcap_geometry_reg_stage(struct vcap_geometry_reg *reg,
	uint16_t width)
{
	reg->width = width;
	reg->staged = 1;
}

/* Takes the staged width for a commit. A staged width serves exactly one
 * commit: a commit with nothing staged returns 0 and the caller rejects
 * it, so a height is never paired with a width an earlier commit used. */
static inline int vcap_geometry_reg_commit(struct vcap_geometry_reg *reg,
	uint16_t *width)
{
	if (!reg->staged)
		return 0;
	*width = reg->width;
	reg->staged = 0;
	return 1;
}

#endif
