/*
 * ZZ9000 SD Boot Metadata Interface
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Parses the Amiga RDB of a FAT-hosted HDF, provides boot metadata
 * to the m68k boot ROM driver via shared buffer and registers.
 */

#ifndef SD_BOOT_H
#define SD_BOOT_H

#include <stdint.h>

#define SD_BOOT_MAGIC           0x5344424F
#define SD_BOOT_VERSION         2
#define SD_BOOT_MAX_PARTITIONS  16
#define SD_BOOT_MAX_FILESYSTEMS 4

/* All multi-byte values below are transported in big-endian byte order
 * so the 68k driver can read them natively. The firmware copies the
 * on-disk bytes pass-through where possible; natively-computed fields
 * get an explicit be32() conversion. */
struct sd_boot_partition {
    uint32_t flags;               /* pb_Flags (bit 0 = bootable) */
    uint32_t environment[20];     /* pb_Environment (DOSEnvec) */
    uint32_t drive_name[8];       /* pb_DriveName (BSTR, 32 bytes) */
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
/* Copy one chunk of the filesystem blob from the internal fs_buf into
 * the shared buffer, starting at byte `offset`, up to `chunk_size`
 * bytes. Short chunks at EOF are fine (remainder ignored). */
int sd_boot_load_fs_chunk(int fs_index, uint32_t offset, uint32_t chunk_size,
                          void *buffer, uint32_t *out_size);

#endif
