/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZ_OVERLAY_COLOR_H
#define ZZ_OVERLAY_COLOR_H

#include <stdint.h>

#include "zz_video_modes.h"

/* Convert a P96 color key from the ARM's in-memory screen representation to
 * the formatter's RGB888 representation.  P96 15/16-bit pixels are big-endian
 * in memory, so an ARM uint16_t load sees the natural RGB555/565 word swapped. */
static inline uint32_t overlay_key_to_rgb(uint32_t native, int colormode)
{
	uint16_t p = (uint16_t)native;
	uint8_t r, g, b;

	if (colormode == MNTVA_COLOR_32BIT)
		return native & 0x00ffffffU;

	p = (uint16_t)((p >> 8) | (p << 8));
	if (colormode == MNTVA_COLOR_16BIT565) {
		r = (uint8_t)((p & 0x1fU) << 3) | (uint8_t)((p & 0x1fU) >> 2);
		g = (uint8_t)(((p >> 5) & 0x3fU) << 2) |
		    (uint8_t)(((p >> 5) & 0x3fU) >> 4);
		b = (uint8_t)(((p >> 11) & 0x1fU) << 3) |
		    (uint8_t)(((p >> 11) & 0x1fU) >> 2);
	} else {
		r = (uint8_t)((p & 0x1fU) << 3) | (uint8_t)((p & 0x1fU) >> 2);
		g = (uint8_t)(((p >> 5) & 0x1fU) << 3) |
		    (uint8_t)(((p >> 5) & 0x1fU) >> 2);
		b = (uint8_t)(((p >> 10) & 0x1fU) << 3) |
		    (uint8_t)(((p >> 10) & 0x1fU) >> 2);
	}
	return ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

#endif /* ZZ_OVERLAY_COLOR_H */
