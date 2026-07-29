/* Native FPGA backend for the P96 packed-YUV422 PIP plane.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "overlay_hw.h"

#include <stdio.h>
#include "video.h"
#include "xaxivdma_hw.h"
#include "xil_io.h"

#define OVERLAY_VDMA_BASE 0x83010000U
#define OVERLAY_RESET_TIMEOUT 100000U

#ifndef ZZ_OVERLAY_HW_PRESENT
#define ZZ_OVERLAY_HW_PRESENT 1
#endif

/* Retain the active transfer shape so each vblank handoff can reset and
 * rebuild MM2S. A live START_ADDR update needs VSIZE to commit, but it can
 * still arrive after the VDMA has prefetched the old frame internally.
 * Resetting at the acknowledged frame boundary discards that stale work. */
static volatile uint32_t overlay_vdma_pitch;
static volatile uint32_t overlay_vdma_line_bytes;
static volatile uint16_t overlay_vdma_height;

int overlay_hw_supported(void)
{
	return ZZ_OVERLAY_HW_PRESENT != 0;
}

static void overlay_vdma_write(uint32_t offset, uint32_t value)
{
	Xil_Out32(OVERLAY_VDMA_BASE + offset, value);
}

static uint32_t overlay_vdma_read(uint32_t offset)
{
	return Xil_In32(OVERLAY_VDMA_BASE + offset);
}

static int overlay_vdma_reset(void)
{
	uint32_t timeout = OVERLAY_RESET_TIMEOUT;

	overlay_vdma_write(XAXIVDMA_CR_OFFSET, XAXIVDMA_CR_RESET_MASK);
	while ((overlay_vdma_read(XAXIVDMA_CR_OFFSET) &
	        XAXIVDMA_CR_RESET_MASK) != 0U && timeout != 0U)
		timeout--;
	if (timeout == 0U) {
		printf("[overlay-hw] VDMA reset timeout\n");
		return 0;
	}
	overlay_vdma_write(XAXIVDMA_SR_OFFSET, XAXIVDMA_SR_ERR_ALL_MASK);
	return 1;
}

static int overlay_vdma_program(uint32_t src_addr, uint32_t src_pitch,
                                uint32_t line_bytes, uint16_t height)
{
	uint32_t addr = XAXIVDMA_MM2S_ADDR_OFFSET;

	if (!overlay_vdma_reset())
		return 0;
	overlay_vdma_write(addr + XAXIVDMA_START_ADDR_OFFSET, src_addr);
	overlay_vdma_write(addr + XAXIVDMA_STRD_FRMDLY_OFFSET, src_pitch);
	overlay_vdma_write(addr + XAXIVDMA_HSIZE_OFFSET, line_bytes);
	overlay_vdma_write(XAXIVDMA_CR_OFFSET,
	                   XAXIVDMA_CR_TAIL_EN_MASK |
	                   XAXIVDMA_CR_RUNSTOP_MASK);
	/* VSIZE is deliberately last: it starts the channel and commits all
	 * direct-register transfer parameters to the internal block. */
	overlay_vdma_write(addr + XAXIVDMA_VSIZE_OFFSET, height);
	return 1;
}

void overlay_hw_stop(void)
{
	/* Invalidate live flips before touching MMIO: a vblank can preempt the
	 * caller until ov.hw_active is cleared and must not recommit the channel
	 * between the reset write and this function's return. */
	overlay_vdma_pitch = 0U;
	overlay_vdma_line_bytes = 0U;
	overlay_vdma_height = 0U;
	if (!overlay_hw_supported())
		return;
	video_formatter_write(0U, MNTVF_OP_OVERLAY_CTRL);
	(void)overlay_vdma_reset();
}

void overlay_hw_set_buffer(uint32_t src_addr, uint32_t generation)
{
	if (!overlay_hw_supported() || !src_addr || !overlay_vdma_pitch ||
	    !overlay_vdma_line_bytes || !overlay_vdma_height)
		return;
	/* The overlay stream is wired directly to the line fetcher. Resetting
	 * here therefore clears every place that could retain an old staging
	 * frame before the generation acknowledgement re-arms line zero. */
	if (!overlay_vdma_program(src_addr, overlay_vdma_pitch,
	                          overlay_vdma_line_bytes,
	                          overlay_vdma_height))
		return;
	video_formatter_write(generation, MNTVF_OP_OVERLAY_FRAME);
}

int overlay_hw_start(uint32_t src_addr, uint32_t src_pitch,
	                 uint16_t width, uint16_t height,
	                 int16_t dst_x, int16_t dst_y,
	                 uint8_t variant, uint32_t key_rgb,
	                 uint8_t key_enabled, uint32_t generation)
{
	uint32_t line_bytes = (((uint32_t)width + 1U) >> 1) * 4U;
	if (!overlay_hw_supported() || !src_addr || !width || !height ||
	    src_pitch < line_bytes ||
	    dst_x < 0 || dst_y < 0)
		return 0;

	overlay_vdma_pitch = 0U;
	overlay_vdma_line_bytes = 0U;
	overlay_vdma_height = 0U;

	/* Hide first: the pixel-domain copy latches this at vblank while the
	 * replacement channel is reset and programmed. */
	video_formatter_write(0U, MNTVF_OP_OVERLAY_CTRL);
	if (!overlay_vdma_program(src_addr, src_pitch, line_bytes, height))
		return 0;
	overlay_vdma_pitch = src_pitch;
	overlay_vdma_line_bytes = line_bytes;
	overlay_vdma_height = height;

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
