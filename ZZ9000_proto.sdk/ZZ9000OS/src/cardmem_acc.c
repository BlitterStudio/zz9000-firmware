/*
 * P96 card-memory acceleration command boundary.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "cardmem_acc.h"
#include "memorymap.h"
#include "surface_allocator.h"

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

    if (!reply_be)
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

    if (!reply_be)
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

    if (!reply_be)
        return 0;
    target = cardmem_be32(target_be);
    *reply_be = 0U;
    if (target > HOST_VISIBLE_HEAP_END - FRAMEBUFFER_ADDRESS)
        return 0;
    return surface_allocator_p96_free(FRAMEBUFFER_ADDRESS + target) == 0;
}
