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

#endif /* CARDMEM_ACC_H */
