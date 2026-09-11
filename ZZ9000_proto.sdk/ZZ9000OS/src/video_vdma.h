#ifndef VIDEO_VDMA_H
#define VIDEO_VDMA_H

#include <stdint.h>
#include "zz_video_modes.h"

/* Framebuffer-relative base of the native capture area (matches the
 * RTL's VIDEOCAP_ADDR write base) and the one legacy exception: the
 * hand-tuned PAL 800x600 filtered scanout origin, which starts 0x2f8
 * bytes (190 words) before the capture rows. */
#define VIDEO_VDMA_CAPTURE_PAN_BASE          0x00e00000U
#define VIDEO_VDMA_CAPTURE_PAN_PAL_800X600   0x00dff2f8U

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

/* Native-scanout origin inside the capture area. The pre-row constant is
 * the tuned centering for the PAL 800x600 filtered profile ONLY. Every
 * other combination must start at the capture row base: a driver's
 * legacy native-pan write of the PAL constant with NTSC detected makes
 * each fetched line begin 190 words before its capture row, wrapping the
 * preceding row's tail across the left edge (zz9000-drivers #84). */
static inline uint32_t video_videocap_scanout_pan_base(uint32_t ntsc,
		uint32_t full_width, uint32_t base_mode)
{
	if (ntsc != 0U || full_width != 0U ||
	    base_mode != ZZVMODE_800x600)
		return video_vdma_native_row_start(
			VIDEO_VDMA_CAPTURE_PAN_BASE);

	return VIDEO_VDMA_CAPTURE_PAN_PAL_800X600;
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
