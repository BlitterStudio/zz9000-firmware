#ifndef VIDEO_SCALE_H
#define VIDEO_SCALE_H

#include <stdint.h>

static inline uint32_t video_vertical_scale_factor(uint32_t scalemode)
{
	return 1U << ((scalemode >> 1) & 3U);
}

static inline uint32_t video_formatter_scale_control(uint32_t scalemode)
{
	/* OP_SCALE uses [2:1] for the vertical shift and [3] for sprite
	 * doubling. Preserve the historical behavior where an x2 vertical
	 * mode also doubled the RTG hardware sprite; x4 videocap does not. */
	return (scalemode & 7U) | ((scalemode & 2U) << 2);
}

#endif
