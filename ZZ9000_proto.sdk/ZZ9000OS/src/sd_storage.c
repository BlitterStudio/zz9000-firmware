/*
 * ZZ9000 SD Card Storage
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Presents a single HDF file on the SD card (FAT32) as a raw block
 * device. All block I/O from the m68k side funnels through a FatFs
 * f_read/f_write on an open HDF file. The SD peripheral is owned by
 * xilffs' disk_initialize — we do not drive XSdPs directly.
 */

#include <stdio.h>
#include <string.h>
#include <ff.h>
#include "xil_cache.h"
#include "sd_storage.h"
#include "memorymap.h"

#define SD_BLOCK_SIZE       512
/* 24 KB max transfer — fills the full Zorro-visible window at
 * cardbase+0xA000..0x10000 in a single round-trip. Must match or
 * exceed the driver's BLOCKS_AT_ONCE in zzsd_cmd.c. */
#define SD_MAX_BLOCKS_AT_ONCE 48

/* Fixed HDF path at the root of the FAT32 volume. Users can rename
 * their image to this filename or symlink it; a config file can come
 * later if we ever want selectable images. */
#define HDF_VOLUME "0:"
#define HDF_PATH   HDF_VOLUME "/zz9000.hdf"

static FATFS fatfs;
static FIL   hdf_file;
static int   hdf_open = 0;
static uint32_t hdf_capacity_blocks = 0;

int sd_storage_init(void) {
    FRESULT fr;

    if (hdf_open) {
        f_close(&hdf_file);
    }
    hdf_open = 0;
    hdf_capacity_blocks = 0;

    fr = f_mount(&fatfs, HDF_VOLUME "/", 1);
    if (fr != FR_OK) {
        printf("[SD] f_mount(%s) failed: %d (no card / not FAT?)\n",
               HDF_VOLUME, (int)fr);
        return -1;
    }

    fr = f_open(&hdf_file, HDF_PATH, FA_READ | FA_WRITE | FA_OPEN_EXISTING);
    if (fr != FR_OK) {
        /* Retry read-only in case the HDF is on a read-only FS or the
         * user left the image locked. Booting still works. */
        fr = f_open(&hdf_file, HDF_PATH, FA_READ | FA_OPEN_EXISTING);
        if (fr != FR_OK) {
            printf("[SD] HDF open (%s) failed: %d\n", HDF_PATH, (int)fr);
            printf("[SD] FAT volume remains mounted for firmware-file push\n");
            return 0;
        }
        printf("[SD] HDF opened read-only\n");
    }

    FSIZE_t size_bytes = f_size(&hdf_file);
    if (size_bytes < SD_BLOCK_SIZE) {
        printf("[SD] HDF too small: %llu bytes\n", (unsigned long long)size_bytes);
        f_close(&hdf_file);
        printf("[SD] FAT volume remains mounted for firmware-file push\n");
        return 0;
    }

    hdf_capacity_blocks = (uint32_t)(size_bytes / SD_BLOCK_SIZE);
    hdf_open = 1;

    printf("[SD] HDF %s mapped, %lu blocks (%lu MB)\n",
           HDF_PATH,
           (unsigned long)hdf_capacity_blocks,
           (unsigned long)(hdf_capacity_blocks / 2048));

    return 0;
}

uint32_t sd_storage_read_blocks(uint32_t block, uint32_t num_blocks, void *buffer) {
    if (!hdf_open) return 0xFF;
    if (num_blocks == 0 || num_blocks > SD_MAX_BLOCKS_AT_ONCE) return 0xFE;
    if ((uint64_t)block + num_blocks > hdf_capacity_blocks) return 0xFC;

    FRESULT fr = f_lseek(&hdf_file, (FSIZE_t)block * SD_BLOCK_SIZE);
    if (fr != FR_OK) {
        printf("[SD] lseek(%lu) failed: %d\n", (unsigned long)block, (int)fr);
        return 0xFD;
    }

    UINT n_read = 0;
    fr = f_read(&hdf_file, buffer, num_blocks * SD_BLOCK_SIZE, &n_read);
    if (fr != FR_OK || n_read != num_blocks * SD_BLOCK_SIZE) {
        printf("[SD] read(block=%lu count=%lu) failed: fr=%d n=%u\n",
               (unsigned long)block, (unsigned long)num_blocks,
               (int)fr, n_read);
        return 0xFD;
    }

    /* f_read writes to `buffer` via CPU (cacheable store); the Zorro
     * read path reads DDR directly (AXI_HP, non-coherent with the ARM
     * D-cache). Clean+invalidate the range so DDR has the fresh block
     * contents before the Amiga fetches them. Using Invalidate alone
     * would discard the just-written cache lines without writing them
     * back, leaving DDR with stale data. */
    Xil_DCacheFlushRange((UINTPTR)buffer, num_blocks * SD_BLOCK_SIZE);
    return 0;
}

uint32_t sd_storage_write_blocks(uint32_t block, uint32_t num_blocks, void *buffer) {
    if (!hdf_open) return 0xFF;
    if (num_blocks == 0 || num_blocks > SD_MAX_BLOCKS_AT_ONCE) return 0xFE;
    if ((uint64_t)block + num_blocks > hdf_capacity_blocks) return 0xFC;

    Xil_DCacheFlushRange((UINTPTR)buffer, num_blocks * SD_BLOCK_SIZE);

    FRESULT fr = f_lseek(&hdf_file, (FSIZE_t)block * SD_BLOCK_SIZE);
    if (fr != FR_OK) {
        printf("[SD] lseek(%lu) failed: %d\n", (unsigned long)block, (int)fr);
        return 0xFD;
    }

    UINT n_written = 0;
    fr = f_write(&hdf_file, buffer, num_blocks * SD_BLOCK_SIZE, &n_written);
    if (fr != FR_OK || n_written != num_blocks * SD_BLOCK_SIZE) {
        printf("[SD] write(block=%lu count=%lu) failed: fr=%d n=%u\n",
               (unsigned long)block, (unsigned long)num_blocks,
               (int)fr, n_written);
        return 0xFD;
    }

    /* Flush cluster/FAT updates immediately so a power loss after a
     * completed write doesn't corrupt the HDF's metadata. A sync
     * failure means the bytes are not durable yet; surface it as a
     * write error instead of silently acking the guest, and close the
     * file so subsequent I/O fails fast rather than continuing against
     * a possibly-torn HDF. The host can power-cycle or remount to
     * recover. */
    fr = f_sync(&hdf_file);
    if (fr != FR_OK) {
        printf("[SD] f_sync(block=%lu count=%lu) failed: fr=%d\n",
               (unsigned long)block, (unsigned long)num_blocks, (int)fr);
        f_close(&hdf_file);
        hdf_open = 0;
        return 0xFD;
    }
    return 0;
}

uint32_t sd_storage_capacity(void) {
    return hdf_capacity_blocks;
}

int sd_storage_available(void) {
    return hdf_open;
}
