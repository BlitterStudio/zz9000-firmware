#ifndef VIDEO_VDMA_H
#define VIDEO_VDMA_H

#include <stdint.h>

#define VIDEO_VDMA_WORD_BYTES 4U
/* Full-width native rows cross the capture-buffer boundary on the A4000.
 * Hardware calibration brackets the required scanout rotation between the
 * unshifted and 128-pixel positions; use the 64-pixel midpoint candidate. */
#define VIDEO_VDMA_FULL_WIDTH_PAN_PIXELS 64U

static inline uint32_t video_vdma_line_bytes(uint32_t hsize, uint32_t hdiv)
{
	if (hdiv == 0U)
		return 0U;

	return (hsize * VIDEO_VDMA_WORD_BYTES) / hdiv;
}

static inline uint32_t video_vdma_pan_right_start(uint32_t start_offset,
						  uint32_t pixels)
{
	if (pixels > (start_offset / VIDEO_VDMA_WORD_BYTES))
		return 0U;

	return start_offset - (pixels * VIDEO_VDMA_WORD_BYTES);
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
