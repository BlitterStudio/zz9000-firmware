/*
 * MNT ZZ9000 SD Boot Metadata Interface
 * Copyright (C) 2026, MNT Research GmbH
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Parses MBR + Amiga RDB from SD card, provides boot metadata
 * to the m68k boot ROM driver via shared buffer and registers.
 */

#ifndef SD_BOOT_H
#define SD_BOOT_H

#include <stdint.h>

#define SD_BOOT_MAGIC           0x5344424F
#define SD_BOOT_VERSION         1
#define SD_BOOT_MAX_PARTITIONS  16
#define SD_BOOT_MAX_FILESYSTEMS 4

struct sd_boot_partition {
    uint32_t start_block;
    uint32_t total_blocks;
    uint32_t dos_type;
    uint32_t flags;
    uint32_t size_block;
    uint32_t surfaces;
    uint32_t sectors_per_block;
    uint32_t blocks_per_track;
    uint32_t reserved;
    uint32_t pre_alloc;
    uint32_t interleave;
    uint32_t low_cyl;
    uint32_t high_cyl;
    uint32_t num_buffer;
    uint32_t buf_mem_type;
    uint32_t max_transfer;
    uint32_t mask;
    uint32_t boot_priority;
    uint32_t drive_name[8];
};

struct sd_boot_filesystem {
    uint32_t dos_type;
    uint32_t version;
    uint32_t patch_flags;
    uint32_t type;
    uint32_t task;
    uint32_t lock;
    uint32_t handler;
    uint32_t stack_size;
    uint32_t priority;
    uint32_t startup;
    uint32_t global_vec;
    uint32_t seg_list_size;
};

struct sd_boot_info {
    uint32_t magic;
    uint32_t version;
    uint32_t partition_count;
    uint32_t filesystem_count;
    uint32_t block_size;
    uint32_t rdb_start_block;
    uint32_t partition_blocks;
    struct sd_boot_partition partitions[SD_BOOT_MAX_PARTITIONS];
    struct sd_boot_filesystem filesystems[SD_BOOT_MAX_FILESYSTEMS];
};

int sd_boot_init(void);
int sd_boot_get_info(void *buffer);
int sd_boot_load_fs(int fs_index, void *buffer, uint32_t *out_size);

#endif
