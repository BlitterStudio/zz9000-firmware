#ifndef VIDEO_VDMA_H
#define VIDEO_VDMA_H

#include <stdint.h>

#define VIDEO_VDMA_WORD_BYTES 4U

static inline uint32_t video_vdma_line_bytes(uint32_t hsize, uint32_t hdiv)
{
	if (hdiv == 0U)
		return 0U;

	return (hsize * VIDEO_VDMA_WORD_BYTES) / hdiv;
}

/* Keep each native scanout line within its corresponding capture row.
 * Starting before the row prepends pixels from the preceding row and drops
 * the same number from the current row; horizontal origin belongs in the
 * capture sampler instead. */
static inline uint32_t video_vdma_native_row_start(uint32_t capture_offset)
{
	return capture_offset;
}

static inline uint32_t video_vdma_stride_bytes(uint32_t hsize, uint32_t hdiv,
                                               uint32_t framebuffer_pan_width,
                                               uint32_t stride_div)
{
	if (hdiv == 0U)
		return 0U;

	if (framebuffer_pan_width != 0U &&
	    framebuffer_pan_width != (hsize / hdiv)) {
		return (framebuffer_pan_width * VIDEO_VDMA_WORD_BYTES *
		        stride_div) / hdiv;
	}

	return video_vdma_line_bytes(hsize, hdiv);
}

#endif
