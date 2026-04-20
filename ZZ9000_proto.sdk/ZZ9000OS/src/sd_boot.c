/*
 * ZZ9000 SD Boot Metadata
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "sd_storage.h"
#include "sd_boot.h"
#include "memorymap.h"

static uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }

static uint8_t block_buf[512];
static struct sd_boot_info boot_info;
static int boot_info_valid = 0;
#define SD_BOOT_FS_BUF_SIZE (64 * 1024)
static uint8_t fs_buf[SD_BOOT_FS_BUF_SIZE];
static uint32_t fs_offsets[SD_BOOT_MAX_FILESYSTEMS];
static uint32_t fs_sizes[SD_BOOT_MAX_FILESYSTEMS];

#define RDB_IDENTIFIER   0x5244534B
#define PART_IDENTIFIER  0x50415254
#define FSHD_IDENTIFIER  0x46534844
#define LSEG_IDENTIFIER  0x4C534547
#define LSEG_DATASIZE    (512 / 4 - 5)

struct RigidDiskBlock {
    uint32_t rdb_ID;
    uint32_t rdb_SummedLongs;
    int32_t  rdb_ChkSum;
    uint32_t rdb_HostID;
    uint32_t rdb_BlockBytes;
    uint32_t rdb_Flags;
    uint32_t rdb_BadBlockList;
    uint32_t rdb_PartitionList;
    uint32_t rdb_FileSysHeaderList;
    uint32_t rdb_DriveInit;
    uint32_t rdb_Reserved1[6];
    uint32_t rdb_Cylinders;
    uint32_t rdb_Sectors;
    uint32_t rdb_Heads;
    uint32_t rdb_Interleave;
    uint32_t rdb_Park;
    uint32_t rdb_Reserved2[3];
    uint32_t rdb_WritePreComp;
    uint32_t rdb_ReducedWrite;
    uint32_t rdb_StepRate;
    uint32_t rdb_Reserved3[5];
    uint32_t rdb_RDBBlocksLo;
    uint32_t rdb_RDBBlocksHi;
    uint32_t rdb_LoCylinder;
    uint32_t rdb_HiCylinder;
    uint32_t rdb_CylBlocks;
    uint32_t rdb_AutoParkSeconds;
    uint32_t rdb_HighRDSKBlock;
    uint32_t rdb_Reserved4;
    char     rdb_DiskVendor[8];
    char     rdb_DiskProduct[16];
    char     rdb_DiskRevision[4];
    char     rdb_ControllerVendor[8];
    char     rdb_ControllerProduct[16];
    char     rdb_ControllerRevision[4];
    char     rdb_DriveInitName[40];
};

struct PartitionBlock {
    uint32_t pb_ID;
    uint32_t pb_SummedLongs;
    int32_t  pb_ChkSum;
    uint32_t pb_HostID;
    uint32_t pb_Next;
    uint32_t pb_Flags;
    uint32_t pb_Reserved1[2];
    uint32_t pb_DevFlags;
    uint8_t  pb_DriveName[32];
    uint32_t pb_Reserved2[15];
    uint32_t pb_Environment[20];
    uint32_t pb_EReserved[12];
};

struct FileSysHeaderBlock {
    uint32_t fhb_ID;
    uint32_t fhb_SummedLongs;
    int32_t  fhb_ChkSum;
    uint32_t fhb_HostID;
    uint32_t fhb_Next;
    uint32_t fhb_Flags;
    uint32_t fhb_Reserved1[2];
    uint32_t fhb_DosType;
    uint32_t fhb_Version;
    uint32_t fhb_PatchFlags;
    uint32_t fhb_Type;
    uint32_t fhb_Task;
    uint32_t fhb_Lock;
    uint32_t fhb_Handler;
    uint32_t fhb_StackSize;
    int32_t  fhb_Priority;
    int32_t  fhb_Startup;
    int32_t  fhb_SegListBlocks;
    int32_t  fhb_GlobalVec;
    uint32_t fhb_Reserved2[23];
    char     fhb_FileSysName[84];
};

struct LoadSegBlock {
    uint32_t lsb_ID;
    uint32_t lsb_SummedLongs;
    int32_t  lsb_ChkSum;
    uint32_t lsb_HostID;
    uint32_t lsb_Next;
    uint32_t lsb_LoadData[LSEG_DATASIZE];
};

int sd_boot_init(void) {
    memset(&boot_info, 0, sizeof(boot_info));
    memset(fs_sizes, 0, sizeof(fs_sizes));
    memset(fs_offsets, 0, sizeof(fs_offsets));
    boot_info_valid = 0;

    if (!sd_storage_available()) {
        printf("[SD] Boot: no HDF mounted\n");
        return -1;
    }

    /* The HDF is presented as the entire Amiga drive — RDSK lives at
     * (or within the first 16 blocks of) offset 0 of the file. */
    uint32_t rdb_start = 0;
    boot_info.partition_blocks = sd_storage_capacity();
    boot_info.rdb_start_block = rdb_start;
    boot_info.block_size = 512;

    /* The Amiga RDB spec lets RDSK sit anywhere in the first 16 blocks of
     * the drive, so scan that range rather than assuming block 0. */
    struct RigidDiskBlock *rdb = (struct RigidDiskBlock *)block_buf;
    int rdb_found = 0;
    uint32_t rdb_block_offset = 0;
    for (uint32_t off = 0; off < 16; off++) {
        if (sd_storage_read_blocks(rdb_start + off, 1, block_buf)) {
            printf("[SD] Boot: read failed at block %lu\n",
                   (unsigned long)(rdb_start + off));
            break;
        }
        if (be32(rdb->rdb_ID) == RDB_IDENTIFIER) {
            rdb_found = 1;
            rdb_block_offset = off;
            break;
        }
    }

    if (!rdb_found) {
        /* Re-read block 0 for a proper dump — the scan loop left block_buf
         * pointing at whatever the last block read was. */
        if (sd_storage_read_blocks(rdb_start, 1, block_buf) == 0) {
            printf("[SD] Boot: block 0 dump:\n");
            for (int row = 0; row < 2; row++) {
                printf("[SD]  %03x: ", row * 16);
                for (int col = 0; col < 16; col++) {
                    printf("%02x ", block_buf[row * 16 + col]);
                }
                printf(" |");
                for (int col = 0; col < 16; col++) {
                    uint8_t c = block_buf[row * 16 + col];
                    printf("%c", (c >= 0x20 && c < 0x7f) ? c : '.');
                }
                printf("|\n");
            }
            /* Classic UAE single-partition hardfiles start with a plain
             * bootblock (DOS\N / PFS\N / SFS\N / PDS\N) instead of an RDB.
             * ZZ9000 needs an RDB-partitioned image. */
            if (block_buf[0] == 'D' && block_buf[1] == 'O' && block_buf[2] == 'S') {
                printf("[SD] Boot: HDF is a UAE single-partition hardfile (DOS\\%u bootblock).\n",
                       block_buf[3]);
                printf("[SD] Boot: ZZ9000 needs an RDB-partitioned hardfile. "
                       "Re-create it in WinUAE/FS-UAE/Amiberry as a 'hardfile' "
                       "with RDB partitioning, or partition it with HDToolBox.\n");
            }
        }

        boot_info.magic = SD_BOOT_MAGIC;
        boot_info.version = SD_BOOT_VERSION;
        boot_info_valid = 1;
        printf("[SD] Boot: no RDSK in first 16 blocks\n");
        printf("[SD] Boot: device ready, no RDB (%lu blocks)\n",
               (unsigned long)boot_info.partition_blocks);
        return 0;
    }

    printf("[SD] Boot: RDSK found at partition block %lu\n",
           (unsigned long)rdb_block_offset);

    if (be32(rdb->rdb_BlockBytes) != 512) {
        boot_info.magic = SD_BOOT_MAGIC;
        boot_info.version = SD_BOOT_VERSION;
        boot_info_valid = 1;
        printf("[SD] Boot: device ready, bad RDB block size\n");
        return 0;
    }

    /* Cache RDB fields we'll need after the partition loop — that loop
     * reuses block_buf for each partition read, so `rdb` (which aliases
     * block_buf) is no longer valid once we're past it. */
    uint32_t rdb_partition_list = be32(rdb->rdb_PartitionList);
    uint32_t rdb_fs_header_list = be32(rdb->rdb_FileSysHeaderList);

    uint32_t part_block = rdb_partition_list;
    int part_count = 0;
    while (part_block != 0 && part_block != 0xFFFFFFFF && part_count < SD_BOOT_MAX_PARTITIONS) {
        if (sd_storage_read_blocks(rdb_start + part_block, 1, block_buf)) break;
        struct PartitionBlock *pb = (struct PartitionBlock *)block_buf;
        if (be32(pb->pb_ID) != PART_IDENTIFIER) break;

        struct sd_boot_partition *sp = &boot_info.partitions[part_count];

        sp->flags = pb->pb_Flags;
        memcpy(sp->environment, pb->pb_Environment, 20 * sizeof(uint32_t));
        memcpy(sp->drive_name, pb->pb_DriveName, 32);

        {
            uint32_t low_cyl = be32(pb->pb_Environment[9]);
            uint32_t high_cyl = be32(pb->pb_Environment[10]);
            uint32_t surfaces = be32(pb->pb_Environment[3]);
            uint32_t blocks_per_track = be32(pb->pb_Environment[5]);
            uint32_t total = (high_cyl >= low_cyl)
                ? (high_cyl - low_cyl + 1) * surfaces * blocks_per_track
                : 0;
            printf("[SD] Partition %d: lowCyl=%lu highCyl=%lu blocks=%lu dostype=%08lx\n",
                   part_count, (unsigned long)low_cyl, (unsigned long)high_cyl,
                   (unsigned long)total,
                   (unsigned long)be32(pb->pb_Environment[16]));
        }

        part_block = be32(pb->pb_Next);
        part_count++;
    }
    boot_info.partition_count = part_count;

    uint32_t fs_block = rdb_fs_header_list;
    int fs_count = 0;
    uint32_t fs_buf_offset = 0;

    printf("[SD] Boot: FSHD list head = %lu\n", (unsigned long)fs_block);

    while (fs_block != 0 && fs_block != 0xFFFFFFFF && fs_count < SD_BOOT_MAX_FILESYSTEMS) {
        if (sd_storage_read_blocks(rdb_start + fs_block, 1, block_buf)) break;
        struct FileSysHeaderBlock *fhb = (struct FileSysHeaderBlock *)block_buf;
        if (be32(fhb->fhb_ID) != FSHD_IDENTIFIER) break;

        struct sd_boot_filesystem *sf = &boot_info.filesystems[fs_count];

        sf->dos_type = fhb->fhb_DosType;
        sf->version = fhb->fhb_Version;
        sf->patch_flags = fhb->fhb_PatchFlags;
        sf->type = fhb->fhb_Type;
        sf->task = fhb->fhb_Task;
        sf->lock = fhb->fhb_Lock;
        sf->handler = fhb->fhb_Handler;
        sf->stack_size = fhb->fhb_StackSize;
        sf->priority = (uint32_t)fhb->fhb_Priority;
        sf->startup = (uint32_t)fhb->fhb_Startup;
        sf->global_vec = (uint32_t)fhb->fhb_GlobalVec;

        uint32_t lseg_block = be32((uint32_t)fhb->fhb_SegListBlocks);
        fs_offsets[fs_count] = fs_buf_offset;
        uint32_t fs_total = 0;

        while (lseg_block != 0 && lseg_block != 0xFFFFFFFF) {
            if (sd_storage_read_blocks(rdb_start + lseg_block, 1, block_buf)) break;
            struct LoadSegBlock *lsb = (struct LoadSegBlock *)block_buf;
            if (be32(lsb->lsb_ID) != LSEG_IDENTIFIER) break;

            uint32_t data_bytes = LSEG_DATASIZE * sizeof(uint32_t);
            if (fs_buf_offset + data_bytes > SD_BOOT_FS_BUF_SIZE) {
                printf("[SD] FS %d: buffer overflow\n", fs_count);
                break;
            }

            memcpy(fs_buf + fs_buf_offset, lsb->lsb_LoadData, data_bytes);
            fs_buf_offset += data_bytes;
            fs_total += data_bytes;

            lseg_block = be32(lsb->lsb_Next);
        }

        fs_sizes[fs_count] = fs_total;
        sf->seg_list_size = fs_total;

        printf("[SD] Filesystem %d: dostype=%08lx size=%lu bytes\n",
               fs_count, (unsigned long)be32(sf->dos_type), (unsigned long)fs_total);

        fs_block = be32(fhb->fhb_Next);
        fs_count++;
    }
    boot_info.filesystem_count = fs_count;

    boot_info.magic = SD_BOOT_MAGIC;
    boot_info.version = SD_BOOT_VERSION;
    boot_info_valid = 1;
    printf("[SD] Boot: %d partitions, %d filesystems found\n", part_count, fs_count);
    return 0;
}

int sd_boot_get_info(void *buffer) {
    if (!boot_info_valid) return -1;

    struct sd_boot_info be;
    memset(&be, 0, sizeof(be));

    be.magic = be32(boot_info.magic);
    be.version = be32(boot_info.version);
    be.partition_count = be32(boot_info.partition_count);
    be.filesystem_count = be32(boot_info.filesystem_count);
    be.block_size = be32(boot_info.block_size);
    be.rdb_start_block = be32(boot_info.rdb_start_block);
    be.partition_blocks = be32(boot_info.partition_blocks);

    for (int i = 0; i < (int)boot_info.partition_count; i++) {
        struct sd_boot_partition *dst = &be.partitions[i];
        struct sd_boot_partition *src = &boot_info.partitions[i];

        /* All per-partition fields are byte-preserved pass-through from
         * the on-disk BE representation, so a plain struct copy is
         * exactly what the 68k side needs to read natively. */
        memcpy(dst, src, sizeof(*dst));
    }

    for (int i = 0; i < (int)boot_info.filesystem_count; i++) {
        struct sd_boot_filesystem *dst = &be.filesystems[i];
        struct sd_boot_filesystem *src = &boot_info.filesystems[i];

        dst->dos_type = src->dos_type;
        dst->version = src->version;
        dst->patch_flags = src->patch_flags;
        dst->type = src->type;
        dst->task = src->task;
        dst->lock = src->lock;
        dst->handler = src->handler;
        dst->stack_size = src->stack_size;
        dst->priority = src->priority;
        dst->startup = src->startup;
        dst->global_vec = src->global_vec;
        dst->seg_list_size = be32(src->seg_list_size);
    }

    memcpy(buffer, &be, sizeof(be));
    return 0;
}

int sd_boot_load_fs_chunk(int fs_index, uint32_t chunk_offset, uint32_t chunk_size,
                          void *buffer, uint32_t *out_size) {
    if (!boot_info_valid || fs_index < 0 || fs_index >= (int)boot_info.filesystem_count)
        return -1;
    if (fs_sizes[fs_index] == 0) return -1;

    uint32_t size = fs_sizes[fs_index];
    uint32_t offset = fs_offsets[fs_index];

    if (chunk_offset >= size) { *out_size = 0; return 0; }
    if (offset + size > SD_BOOT_FS_BUF_SIZE) return -1;

    uint32_t remaining = size - chunk_offset;
    uint32_t n = remaining < chunk_size ? remaining : chunk_size;

    memcpy(buffer, fs_buf + offset + chunk_offset, n);
    *out_size = n;
    return 0;
}
