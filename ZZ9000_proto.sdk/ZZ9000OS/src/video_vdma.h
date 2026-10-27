#ifndef VIDEO_VDMA_H
#define VIDEO_VDMA_H

#include <stdint.h>
#include "zz_video_modes.h"

/* The sampler writes up to a 1280-word by 1024-row, 32-bit frame. Capture
 * backing is allocated from host-visible DDR and must meet the RTL's 64-byte
 * address-alignment requirement. */
#define VIDEO_VDMA_WORD_BYTES 4U
#define VIDEO_VIDEOCAP_MAX_WORDS          1280U
#define VIDEO_VIDEOCAP_MAX_ROWS           1024U
#define VIDEO_VIDEOCAP_BACKING_ALIGNMENT  64U
#define VIDEO_VIDEOCAP_BACKING_SIZE \
	(VIDEO_VIDEOCAP_MAX_WORDS * VIDEO_VDMA_WORD_BYTES * \
	 VIDEO_VIDEOCAP_MAX_ROWS)

/* PAL filtered 800x600 preserves its tuned pre-row scanout origin. This is
 * deliberately a delta from the runtime capture backing rather than a fixed
 * DDR address. */
#define VIDEO_VIDEOCAP_PAL_800X600_SCANOUT_DELTA 0x00000d08U
#define VIDEO_VIDEOCAP_BACKING_RESERVATION_SIZE \
	(VIDEO_VIDEOCAP_BACKING_SIZE + \
	 VIDEO_VIDEOCAP_PAL_800X600_SCANOUT_DELTA + \
	 VIDEO_VIDEOCAP_BACKING_ALIGNMENT - 1U)

static inline uint32_t video_vdma_line_bytes(uint32_t hsize, uint32_t hdiv)
{
	if (hdiv == 0U)
		return 0U;

	return (hsize * VIDEO_VDMA_WORD_BYTES) / hdiv;
}

static inline uint32_t video_vdma_native_row_start(uint32_t capture_address)
{
	return capture_address;
}

static inline uint32_t video_videocap_scanout_address(uint32_t capture_address,
		uint32_t ntsc, uint32_t full_width, uint32_t base_mode)
{
	if (ntsc != 0U || full_width != 0U ||
	    base_mode != ZZVMODE_800x600)
		return video_vdma_native_row_start(capture_address);

	return capture_address - VIDEO_VIDEOCAP_PAL_800X600_SCANOUT_DELTA;
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
