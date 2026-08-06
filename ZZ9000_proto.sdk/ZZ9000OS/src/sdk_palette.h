/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Readable shadow of the primary display CLUT.
 *
 * The video formatter's palette registers are write-only, so the RTG palette
 * upload path keeps a mirror here and SDK_OP_QUERY_PALETTE reads it back.
 * This unit deliberately has no Xilinx or video dependencies so the packing
 * and bounds logic that runs on the card can be tested on the host.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ_SDK_PALETTE_H
#define ZZ_SDK_PALETTE_H

#include <stdint.h>

#define SDK_PALETTE_ENTRIES 256U
#define SDK_PALETTE_ENTRY_BYTES 4U

/* Records one primary-CLUT entry. `xrgb` may carry the index in its high
 * byte as the hardware write does; only the low 24 bits are retained. */
void sdk_palette_set(uint32_t idx, uint32_t xrgb);

/* Returns entry `idx` as 0x00RRGGBB. Indices never uploaded since boot read
 * back as 0, which the ABI documents. */
uint32_t sdk_palette_get(uint32_t idx);

/* Writes `count` entries from `start` to `dst` as consecutive big-endian
 * 0x00RRGGBB words, matching the mailbox wire format. Returns the number of
 * bytes written, or 0 if the window is out of bounds or `dst` is NULL --
 * in which case nothing is written. */
uint32_t sdk_palette_pack_be(void *dst, uint32_t start, uint32_t count);

/* Test seam: drops every shadowed entry back to 0. */
void sdk_palette_reset(void);

#endif /* ZZ_SDK_PALETTE_H */
