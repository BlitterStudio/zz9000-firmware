/*
 * Host-side checks for the native-overlay VDMA programming sequence.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>

#include "overlay_hw.h"
#include "video.h"
#include "xaxivdma_hw.h"

#define OVERLAY_VDMA_BASE 0x83010000U
#define MAX_WRITES 32U

struct mmio_write {
	uintptr_t address;
	uint32_t value;
};

static struct mmio_write writes[MAX_WRITES];
static unsigned write_count;
static uint32_t formatter_data;
static uint16_t formatter_op;
static unsigned formatter_count;
static int probe_flip_on_reset;
static int reset_stuck;
static uint32_t status_value;

void test_xil_out32(uintptr_t address, uint32_t value)
{
	if (write_count < MAX_WRITES) {
		writes[write_count].address = address;
		writes[write_count].value = value;
	}
	write_count++;
	if (probe_flip_on_reset &&
	    address == OVERLAY_VDMA_BASE + XAXIVDMA_CR_OFFSET &&
	    value == XAXIVDMA_CR_RESET_MASK) {
		/* Model a vblank arriving immediately after the stop reset write. */
		probe_flip_on_reset = 0;
		overlay_hw_set_buffer(0x12000000U, 3U);
	}
}

uint32_t test_xil_in32(uintptr_t address)
{
	if (address == OVERLAY_VDMA_BASE + XAXIVDMA_SR_OFFSET)
		return status_value;
	if (address == OVERLAY_VDMA_BASE + XAXIVDMA_CR_OFFSET)
		return reset_stuck ? XAXIVDMA_CR_RESET_MASK : 0U;
	return 0U;
}

void video_formatter_write(uint32_t data, uint16_t op)
{
	formatter_data = data;
	formatter_op = op;
	formatter_count++;
}

static void clear_log(void)
{
	write_count = 0U;
	formatter_data = 0U;
	formatter_op = 0U;
	formatter_count = 0U;
}

static int expect_write(unsigned index, uintptr_t address, uint32_t value)
{
	if (index >= write_count || writes[index].address != address ||
	    writes[index].value != value) {
		printf("write %u: got %08lx=%08lx expected %08lx=%08lx\n",
		       index,
		       index < write_count ? (unsigned long)writes[index].address : 0UL,
		       index < write_count ? (unsigned long)writes[index].value : 0UL,
		       (unsigned long)address, (unsigned long)value);
		return 0;
	}
	return 1;
}

int main(void)
{
	const uintptr_t channel = OVERLAY_VDMA_BASE +
	                          XAXIVDMA_MM2S_ADDR_OFFSET;

	if (!overlay_hw_start(0x10000000U, 1280U, 640U, 480U,
	                      12, 8, 0U, 0U, 0U, 1U)) {
		printf("overlay start failed\n");
		return 1;
	}

	clear_log();
	status_value = XAXIVDMA_SR_IDLE_MASK;
	if (!overlay_hw_set_buffer(0x11000000U, 2U))
		return 2;
	if (write_count != 4U ||
	    !expect_write(0U, channel + XAXIVDMA_START_ADDR_OFFSET,
	                  0x11000000U) ||
	    !expect_write(1U, channel + XAXIVDMA_STRD_FRMDLY_OFFSET, 1280U) ||
	    !expect_write(2U, channel + XAXIVDMA_HSIZE_OFFSET, 1280U) ||
	    !expect_write(3U, channel + XAXIVDMA_VSIZE_OFFSET, 480U)) {
		return 3;
	}
	if (formatter_count != 1U || formatter_op != MNTVF_OP_OVERLAY_FRAME ||
	    formatter_data != 2U) {
		printf("generation commit: got count=%u op=%u data=%lu\n",
		       formatter_count, formatter_op,
		       (unsigned long)formatter_data);
		return 4;
	}

	clear_log();
	status_value = 0U; /* RUNSTOP is still active: prior frame is late. */
	if (overlay_hw_set_buffer(0x11400000U, 3U) ||
	    write_count != 0U || formatter_count != 0U) {
		printf("busy channel was reset or advanced\n");
		return 5;
	}

	clear_log();
	status_value = XAXIVDMA_SR_ERR_INTERNAL_MASK;
	reset_stuck = 1;
	if (overlay_hw_set_buffer(0x11800000U, 3U))
		return 6;
	reset_stuck = 0;
	status_value = 0U;
	if (write_count != 1U ||
	    !expect_write(0U, OVERLAY_VDMA_BASE + XAXIVDMA_CR_OFFSET,
	                  XAXIVDMA_CR_RESET_MASK) ||
	    formatter_count != 0U) {
		printf("timed-out reset advanced the generation\n");
		return 7;
	}

	clear_log();
	probe_flip_on_reset = 1;
	overlay_hw_stop();
	if (write_count != 2U ||
	    !expect_write(0U, OVERLAY_VDMA_BASE + XAXIVDMA_CR_OFFSET,
	                  XAXIVDMA_CR_RESET_MASK) ||
	    !expect_write(1U, OVERLAY_VDMA_BASE + XAXIVDMA_SR_OFFSET,
	                  XAXIVDMA_SR_ERR_ALL_MASK) ||
	    formatter_count != 1U || formatter_op != MNTVF_OP_OVERLAY_CTRL ||
	    formatter_data != 0U) {
		printf("stop allowed a re-entrant buffer commit\n");
		return 8;
	}

	clear_log();
	overlay_hw_set_buffer(0x12000000U, 3U);
	if (write_count != 0U || formatter_count != 0U) {
		printf("stopped channel accepted a buffer flip\n");
		return 9;
	}

	printf("overlay_hw_test: PASS\n");
	return 0;
}
