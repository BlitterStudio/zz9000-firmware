/*
 * MNT ZZ9000 SD Card Storage Interface
 * Copyright (C) 2026, MNT Research GmbH
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SD_STORAGE_H
#define SD_STORAGE_H

#include <stdint.h>

int sd_storage_init(void);
uint32_t sd_storage_read_blocks(uint32_t block, uint32_t num_blocks, void *buffer);
uint32_t sd_storage_write_blocks(uint32_t block, uint32_t num_blocks, void *buffer);
uint32_t sd_storage_capacity(void);
int sd_storage_available(void);

#endif
