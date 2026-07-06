/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Cortex-A9 cache maintenance for the LZH dtext reset-time reclaim state.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "xil_cache.h"
#include "zz9k_lzh.h"

void
zz9k_lzh_flush_dtext_reclaim(void)
{
    Xil_DCacheFlushRange((INTPTR)zz9k_lzh_dtext_reclaim_base(),
                         zz9k_lzh_dtext_reclaim_bytes());
}

void
zz9k_lzh_invalidate_dtext_reclaim(void)
{
    Xil_DCacheInvalidateRange((INTPTR)zz9k_lzh_dtext_reclaim_base(),
                              zz9k_lzh_dtext_reclaim_bytes());
}
