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

static inline uint32_t video_videocap_full_width(uint32_t requested,
		uint32_t fullrate_capable)
{
	/* A full-width request is only safe when the loaded bitstream has the
	 * full-rate sampler/writeback path. Filtered-only variants otherwise
	 * leave the unused tail of each 1280-pixel row stale. */
	return (requested != 0U) && (fullrate_capable != 0U);
}

static inline uint32_t video_videocap_scalemode(uint32_t full_width,
		uint32_t interlace)
{
	/* Full-width progressive capture needs x4 to fill 1024 lines;
	 * filtered capture retains the legacy x2 path. Interlaced input
	 * already supplies twice as many source lines. */
	return full_width ? (interlace ? 2U : 4U)
	                  : (interlace ? 0U : 2U);
}

#endif
