/*
 * P96 card-memory acceleration command boundary.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>

#include "cardmem_acc.h"
#include "memorymap.h"
#include "surface_allocator.h"

static uint8_t cardmem_acc_enabled;
static uint8_t cardmem_acc_refusal_logged;

void cardmem_acc_reset(void)
{
    cardmem_acc_enabled = 0U;
    cardmem_acc_refusal_logged = 0U;
}

int cardmem_acc_announce(int is_zorro3)
{
    if (!is_zorro3)
        return 0;
    if (cardmem_acc_enabled)
        return 1;
    if (surface_allocator_release_reservation(FRAMEBUFFER_ADDRESS) != 0)
        return 0;
    surface_allocator_set_dynamic_floor(FRAMEBUFFER_ADDRESS +
                                        CARDMEM_P96_SPLIT_SLOT_SIZE);
    cardmem_acc_enabled = 1U;
    return 1;
}

int cardmem_acc_is_enabled(void)
{
    return cardmem_acc_enabled != 0U;
}

static uint32_t cardmem_be32(uint32_t value)
{
    return ((value & 0x000000ffU) << 24) |
           ((value & 0x0000ff00U) << 8) |
           ((value & 0x00ff0000U) >> 8) |
           ((value & 0xff000000U) >> 24);
}

int cardmem_acc_alloc(uint32_t alignment_be, uint32_t size_be,
                      uint32_t *reply_be)
{
    uint32_t address;

    if (!reply_be || !cardmem_acc_enabled)
        return 0;
    address = surface_allocator_p96_alloc(cardmem_be32(size_be),
                                          cardmem_be32(alignment_be));
    if (address == 0U)
        goto failed;
    *reply_be = cardmem_be32(address - FRAMEBUFFER_ADDRESS);
    return 1;

failed:
    *reply_be = 0U;
    return 0;
}

int cardmem_acc_alloc_abs(uint32_t target_be, uint32_t size_be,
                          uint32_t *reply_be)
{
    uint32_t target;
    uint32_t size;
    uint32_t address;

    if (!reply_be || !cardmem_acc_enabled)
        return 0;
    target = cardmem_be32(target_be);
    size = cardmem_be32(size_be);
    if (target > HOST_VISIBLE_HEAP_END - FRAMEBUFFER_ADDRESS)
        goto failed;
    address = surface_allocator_p96_alloc_abs(FRAMEBUFFER_ADDRESS + target,
                                              size);
    if (address == 0U)
        goto failed;
    *reply_be = target_be;
    return 1;

failed:
    *reply_be = 0U;
    return 0;
}

int cardmem_acc_free(uint32_t target_be, uint32_t *reply_be)
{
    uint32_t target;

    if (!reply_be || !cardmem_acc_enabled)
        return 0;
    target = cardmem_be32(target_be);
    *reply_be = 0U;
    if (target > HOST_VISIBLE_HEAP_END - FRAMEBUFFER_ADDRESS)
        return 0;
    return surface_allocator_p96_free(FRAMEBUFFER_ADDRESS + target) == 0;
}

void cardmem_acc_dispatch(enum cardmem_acc_command command,
			  uint32_t *offset0_be, uint32_t *offset1_be,
			  uint8_t *accepted)
{
	uint32_t reply = 0U;
	int result = 0;

	if (!offset0_be || !accepted)
		return;
	*accepted = 0U;
	if (!cardmem_acc_enabled && !cardmem_acc_refusal_logged) {
		/* A driver that skipped or failed the handoff is still routing
		 * P96 allocations here; say so once per Amiga reset. */
		printf("[p96] cardmem: op %d before handoff refused\n",
		       (int)command);
		cardmem_acc_refusal_logged = 1U;
	}
	switch (command) {
	case CARDMEM_ACC_COMMAND_ALLOC:
		if (offset1_be)
			result = cardmem_acc_alloc(*offset0_be, *offset1_be, &reply);
		break;
	case CARDMEM_ACC_COMMAND_ALLOC_ABS:
		if (offset1_be)
			result = cardmem_acc_alloc_abs(*offset0_be, *offset1_be, &reply);
		break;
	case CARDMEM_ACC_COMMAND_FREE:
		result = cardmem_acc_free(*offset0_be, &reply);
		break;
	default:
		break;
	}
	*offset0_be = reply;
	*accepted = result ? 1U : 0U;
}
