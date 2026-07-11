/* Native FPGA backend for the P96 packed-YUV422 PIP plane.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "overlay_hw.h"

#include <stdio.h>
#include "video.h"
#include "xaxivdma_hw.h"
#include "xil_io.h"

#define OVERLAY_VDMA_BASE 0x83010000U
#define OVERLAY_RESET_TIMEOUT 100000U

static void overlay_vdma_write(uint32_t offset, uint32_t value)
{
	Xil_Out32(OVERLAY_VDMA_BASE + offset, value);
}

static uint32_t overlay_vdma_read(uint32_t offset)
{
	return Xil_In32(OVERLAY_VDMA_BASE + offset);
}

void overlay_hw_stop(void)
{
	video_formatter_write(0U, MNTVF_OP_OVERLAY_CTRL);
	overlay_vdma_write(XAXIVDMA_CR_OFFSET, XAXIVDMA_CR_RESET_MASK);
}

void overlay_hw_set_buffer(uint32_t src_addr, uint32_t generation)
{
	/* With one circular frame store, changing START_ADDR while MM2S is
	 * backpressured at the frame boundary selects the next frame.  The
	 * generation op simultaneously invalidates prefetched PL line tags. */
	overlay_vdma_write(XAXIVDMA_MM2S_ADDR_OFFSET +
	                   XAXIVDMA_START_ADDR_OFFSET, src_addr);
	video_formatter_write(generation, MNTVF_OP_OVERLAY_FRAME);
}

int overlay_hw_start(uint32_t src_addr, uint32_t src_pitch,
	                 uint16_t width, uint16_t height,
	                 int16_t dst_x, int16_t dst_y,
	                 uint8_t variant, uint32_t key_rgb,
	                 uint8_t key_enabled, uint32_t generation)
{
	uint32_t line_bytes = (((uint32_t)width + 1U) >> 1) * 4U;
	uint32_t timeout = OVERLAY_RESET_TIMEOUT;
	uint32_t addr = XAXIVDMA_MM2S_ADDR_OFFSET;

	if (!src_addr || !width || !height || src_pitch < line_bytes ||
	    dst_x < 0 || dst_y < 0)
		return 0;

	/* Hide first: the pixel-domain copy latches this at vblank while the
	 * replacement channel is reset and programmed. */
	video_formatter_write(0U, MNTVF_OP_OVERLAY_CTRL);
	overlay_vdma_write(XAXIVDMA_CR_OFFSET, XAXIVDMA_CR_RESET_MASK);
	while ((overlay_vdma_read(XAXIVDMA_CR_OFFSET) &
	        XAXIVDMA_CR_RESET_MASK) != 0U && timeout != 0U)
		timeout--;
	if (timeout == 0U) {
		printf("[overlay-hw] VDMA reset timeout\n");
		return 0;
	}

	/* Clear sticky channel errors, configure one circular frame store, then
	 * write VSIZE last to launch direct-register mode. */
	overlay_vdma_write(XAXIVDMA_SR_OFFSET, XAXIVDMA_SR_ERR_ALL_MASK);
	overlay_vdma_write(addr + XAXIVDMA_START_ADDR_OFFSET, src_addr);
	overlay_vdma_write(addr + XAXIVDMA_STRD_FRMDLY_OFFSET, src_pitch);
	overlay_vdma_write(addr + XAXIVDMA_HSIZE_OFFSET, line_bytes);
	overlay_vdma_write(XAXIVDMA_CR_OFFSET,
	                   XAXIVDMA_CR_TAIL_EN_MASK |
	                   XAXIVDMA_CR_RUNSTOP_MASK);
	overlay_vdma_write(addr + XAXIVDMA_VSIZE_OFFSET, height);

	video_formatter_write(((uint32_t)(uint16_t)dst_y << 16) |
	                      (uint16_t)dst_x, MNTVF_OP_OVERLAY_POS);
	video_formatter_write(((uint32_t)height << 16) | width,
	                      MNTVF_OP_OVERLAY_SIZE);
	video_formatter_write(key_rgb & 0x00ffffffU,
	                      MNTVF_OP_OVERLAY_KEY);
	video_formatter_write(((uint32_t)height << 16) | width,
	                      MNTVF_OP_OVERLAY_SOURCE_SIZE);
	video_formatter_write(generation, MNTVF_OP_OVERLAY_FRAME);
	video_formatter_write(1U | ((uint32_t)(key_enabled != 0U) << 1) |
	                      ((uint32_t)variant << 4),
	                      MNTVF_OP_OVERLAY_CTRL);
	return 1;
}
