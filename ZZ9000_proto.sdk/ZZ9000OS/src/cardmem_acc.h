/*
 * P96 card-memory acceleration command boundary.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef CARDMEM_ACC_H
#define CARDMEM_ACC_H

#include <stdint.h>

/* All arguments and replies are command-boundary big-endian words. The
 * return value tells the dispatcher whether the zero offset is a valid grant
 * (status 1) or an allocation/free failure (status 0). */
int cardmem_acc_alloc(uint32_t alignment_be, uint32_t size_be,
                      uint32_t *reply_be);
int cardmem_acc_alloc_abs(uint32_t target_be, uint32_t size_be,
                          uint32_t *reply_be);
int cardmem_acc_free(uint32_t target_be, uint32_t *reply_be);

enum cardmem_acc_command {
	CARDMEM_ACC_COMMAND_ALLOC,
	CARDMEM_ACC_COMMAND_ALLOC_ABS,
	CARDMEM_ACC_COMMAND_FREE,
};

/* Apply a CARDMEM wire command exactly as dma_acc.c does: accepted reports
 * success separately because offset zero is a valid board-relative reply. */
void cardmem_acc_dispatch(enum cardmem_acc_command command,
			  uint32_t *offset0_be, uint32_t *offset1_be,
			  uint8_t *accepted);

#endif /* CARDMEM_ACC_H */
