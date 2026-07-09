/*
 * ZZ9000 SD Card Storage
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Presents a single HDF file on the SD card (FAT32) as a raw block
 * device. At init the HDF's cluster chain is resolved into an extent
 * map; block I/O from the m68k side then goes straight to
 * disk_read/disk_write at physical LBAs (FatFs f_read/f_write only as
 * a fallback for unmappable files). The SD peripheral is owned by
 * xilffs' disk_initialize — we do not drive XSdPs directly.
 */

#include <stdio.h>
#include <string.h>
#include <ff.h>
#include <diskio.h>
#include "xil_cache.h"
#include "sd_activity_led.h"
#include "sd_storage.h"
#include "zz_config.h"
#include "memorymap.h"

#define SD_BLOCK_SIZE       512
/* 24 KB max transfer — fills the full Zorro-visible window at
 * cardbase+0xA000..0x10000 in a single round-trip. Must match or
 * exceed the driver's BLOCKS_AT_ONCE in zzsd_cmd.c. */
#define SD_MAX_BLOCKS_AT_ONCE 48

/* Default HDF path at the root of the FAT32 volume; a `hdf = name`
 * line in ZZ9000.CFG selects a different image. */
#define HDF_VOLUME "0:"
#define HDF_PATH   HDF_VOLUME "/zz9000.hdf"

static const char* hdf_path(void) {
    const struct zz_config *cfg = zz_config_get();
    return cfg->hdf_present ? cfg->hdf_path : HDF_PATH;
}

static FATFS fatfs;
static FIL   hdf_file;
static int   hdf_open = 0;
static int   hdf_read_only = 0;
static uint32_t hdf_capacity_blocks = 0;

/* Raw block I/O fast path.
 *
 * Going through f_lseek/f_read/f_write for every guest request is
 * pathologically slow: the BSP's FatFs is built with FF_USE_FASTSEEK=0,
 * so any seek to a lower file offset rewinds to the file's first
 * cluster and re-walks the whole FAT chain through the one-sector
 * fs->win cache — hundreds of SD commands per seek on a multi-GB HDF,
 * and Amiga filesystems seek backwards (bitmap <-> data) all the time.
 *
 * Instead we resolve the HDF's cluster chain once at init into a table
 * of contiguous extents and service all block I/O with direct
 * multi-sector disk_read()/disk_write() at physical LBAs. The HDF is
 * opened FA_OPEN_EXISTING and never grows, so raw data writes touch no
 * FAT/directory metadata and need no f_sync. If the chain can't be
 * mapped (FAT12, corrupt chain, or more extents than the table) we
 * fall back to the old FatFs path. */
#define HDF_MAX_EXTENTS 1024
struct hdf_extent {
    uint32_t vblock;  /* first logical 512-byte block of this extent */
    uint32_t lba;     /* physical sector on the SD card */
    uint32_t count;   /* extent length in sectors */
};
static struct hdf_extent hdf_extents[HDF_MAX_EXTENTS];
static uint32_t hdf_extent_count = 0;
static uint8_t  hdf_pdrv = 0;
static int      hdf_raw = 0;

/* FAT window for the one-time chain walk; 32 sectors per disk_read
 * keeps the walk to a handful of SD commands even for large FATs. */
#define FAT_WIN_SECTORS 32
static uint8_t fat_win[FAT_WIN_SECTORS * SD_BLOCK_SIZE] __attribute__((aligned(32)));

static int fat_next(FATFS *fs, uint32_t fat_win_state[2], uint32_t clst, uint32_t *next) {
    uint32_t entry_bytes = (fs->fs_type == FS_FAT32) ? 4 : 2;
    uint32_t byte_off = clst * entry_bytes;
    uint32_t sect = fs->fatbase + byte_off / SD_BLOCK_SIZE;
    uint32_t win_sect = fat_win_state[0];
    uint32_t win_count = fat_win_state[1];

    if (win_count == 0 || sect < win_sect || sect >= win_sect + win_count) {
        uint32_t fat_end = fs->fatbase + fs->fsize;
        uint32_t n = FAT_WIN_SECTORS;
        if (sect >= fat_end) return -1;
        if (sect + n > fat_end) n = fat_end - sect;
        if (disk_read(fs->pdrv, fat_win, sect, n) != RES_OK) return -1;
        fat_win_state[0] = win_sect = sect;
        fat_win_state[1] = win_count = n;
    }

    const uint8_t *p = fat_win + (sect - win_sect) * SD_BLOCK_SIZE
                     + (byte_off % SD_BLOCK_SIZE);
    if (fs->fs_type == FS_FAT32) {
        *next = ((uint32_t)p[0] | (uint32_t)p[1] << 8 |
                 (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24) & 0x0FFFFFFF;
    } else {
        *next = (uint32_t)p[0] | (uint32_t)p[1] << 8;
    }
    return 0;
}

static int hdf_push_extent(FATFS *fs, uint32_t run_first, uint32_t run_len,
                           uint32_t *covered) {
    if (hdf_extent_count >= HDF_MAX_EXTENTS) return -1;
    struct hdf_extent *e = &hdf_extents[hdf_extent_count++];
    e->vblock = *covered;
    e->lba = fs->database + (run_first - 2) * fs->csize;
    e->count = run_len * fs->csize;
    *covered += e->count;
    return 0;
}

static int hdf_build_extent_map(void) {
    FATFS *fs = hdf_file.obj.fs;
    uint32_t fat_win_state[2] = {0, 0};

    hdf_extent_count = 0;

    if (fs->fs_type != FS_FAT16 && fs->fs_type != FS_FAT32) return -1;

    uint32_t clst = hdf_file.obj.sclust;
    if (clst < 2 || clst >= fs->n_fatent) return -1;

    uint32_t covered = 0;      /* logical sectors mapped so far */
    uint32_t run_first = clst; /* first cluster of the current run */
    uint32_t run_len = 1;      /* clusters in the current run */

    while (covered + run_len * fs->csize < hdf_capacity_blocks) {
        uint32_t next;
        if (fat_next(fs, fat_win_state, clst, &next)) return -1;
        if (next == clst + 1 && next < fs->n_fatent) {
            run_len++;
            clst = next;
            continue;
        }
        if (hdf_push_extent(fs, run_first, run_len, &covered)) return -1;
        /* Chain must continue — the file size says more clusters follow. */
        if (next < 2 || next >= fs->n_fatent) return -1;
        run_first = next;
        run_len = 1;
        clst = next;
    }
    if (hdf_push_extent(fs, run_first, run_len, &covered)) return -1;

    hdf_pdrv = fs->pdrv;
    return 0;
}

static uint32_t hdf_raw_io(uint32_t block, uint32_t num_blocks, uint8_t *buffer,
                           int is_write) {
    while (num_blocks) {
        /* Binary search for the extent containing `block` (extents are
         * sorted by vblock and cover the whole HDF). */
        uint32_t lo = 0, hi = hdf_extent_count - 1;
        while (lo < hi) {
            uint32_t mid = (lo + hi + 1) / 2;
            if (hdf_extents[mid].vblock <= block) lo = mid; else hi = mid - 1;
        }
        struct hdf_extent *e = &hdf_extents[lo];
        uint32_t off = block - e->vblock;
        uint32_t n = e->count - off;
        if (n > num_blocks) n = num_blocks;

        DRESULT dr = is_write
            ? disk_write(hdf_pdrv, buffer, e->lba + off, n)
            : disk_read(hdf_pdrv, buffer, e->lba + off, n);
        if (dr != RES_OK) {
            printf("[SD] raw %s(block=%lu count=%lu lba=%lu) failed: %d\n",
                   is_write ? "write" : "read",
                   (unsigned long)block, (unsigned long)n,
                   (unsigned long)(e->lba + off), (int)dr);
            return 0xFD;
        }
        block += n;
        buffer += n * SD_BLOCK_SIZE;
        num_blocks -= n;
    }
    return 0;
}

int sd_storage_init(void) {
    FRESULT fr;

    if (hdf_open) {
        f_close(&hdf_file);
    }
    hdf_open = 0;
    hdf_read_only = 0;
    hdf_raw = 0;
    hdf_capacity_blocks = 0;

    fr = f_mount(&fatfs, HDF_VOLUME "/", 1);
    if (fr != FR_OK) {
        printf("[SD] f_mount(%s) failed: %d (no card / not FAT?)\n",
               HDF_VOLUME, (int)fr);
        return -1;
    }

    const char *path = hdf_path();
    fr = f_open(&hdf_file, path, FA_READ | FA_WRITE | FA_OPEN_EXISTING);
    if (fr != FR_OK) {
        /* Retry read-only in case the HDF is on a read-only FS or the
         * user left the image locked. Booting still works. */
        fr = f_open(&hdf_file, path, FA_READ | FA_OPEN_EXISTING);
        if (fr != FR_OK) {
            printf("[SD] HDF open (%s) failed: %d\n", path, (int)fr);
            printf("[SD] FAT volume remains mounted for firmware-file push\n");
            return 0;
        }
        printf("[SD] HDF opened read-only\n");
        hdf_read_only = 1;
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
           path,
           (unsigned long)hdf_capacity_blocks,
           (unsigned long)(hdf_capacity_blocks / 2048));

    if (hdf_build_extent_map() == 0) {
        hdf_raw = 1;
        printf("[SD] HDF extent map: %lu extent(s), raw block I/O enabled\n",
               (unsigned long)hdf_extent_count);
    } else {
        printf("[SD] HDF not extent-mappable (FAT12/fragmented/corrupt chain), "
               "using FatFs I/O — expect degraded speed. Re-copying the HDF "
               "onto a freshly formatted card restores the fast path.\n");
    }

    return 0;
}

uint32_t sd_storage_read_blocks(uint32_t block, uint32_t num_blocks, void *buffer) {
    if (!hdf_open) return 0xFF;
    if (num_blocks == 0 || num_blocks > SD_MAX_BLOCKS_AT_ONCE) return 0xFE;
    if ((uint64_t)block + num_blocks > hdf_capacity_blocks) return 0xFC;

    uint32_t status = 0;
    sd_activity_led_begin();

    if (hdf_raw) {
        /* XSdPs DMAs straight into DDR and invalidates the ARM D-cache
         * range itself, so the Zorro AXI read path sees fresh bytes
         * without an explicit flush here. */
        status = hdf_raw_io(block, num_blocks, buffer, 0);
        goto out;
    }

    FRESULT fr = f_lseek(&hdf_file, (FSIZE_t)block * SD_BLOCK_SIZE);
    if (fr != FR_OK) {
        printf("[SD] lseek(%lu) failed: %d\n", (unsigned long)block, (int)fr);
        status = 0xFD;
        goto out;
    }

    UINT n_read = 0;
    fr = f_read(&hdf_file, buffer, num_blocks * SD_BLOCK_SIZE, &n_read);
    if (fr != FR_OK || n_read != num_blocks * SD_BLOCK_SIZE) {
        printf("[SD] read(block=%lu count=%lu) failed: fr=%d n=%u\n",
               (unsigned long)block, (unsigned long)num_blocks,
               (int)fr, n_read);
        status = 0xFD;
        goto out;
    }

    /* f_read writes to `buffer` via CPU (cacheable store); the Zorro
     * read path reads DDR directly (AXI_HP, non-coherent with the ARM
     * D-cache). Clean+invalidate the range so DDR has the fresh block
     * contents before the Amiga fetches them. Using Invalidate alone
     * would discard the just-written cache lines without writing them
     * back, leaving DDR with stale data. */
    Xil_DCacheFlushRange((UINTPTR)buffer, num_blocks * SD_BLOCK_SIZE);

out:
    sd_activity_led_end();
    return status;
}

uint32_t sd_storage_write_blocks(uint32_t block, uint32_t num_blocks, void *buffer) {
    if (!hdf_open) return 0xFF;
    if (num_blocks == 0 || num_blocks > SD_MAX_BLOCKS_AT_ONCE) return 0xFE;
    if ((uint64_t)block + num_blocks > hdf_capacity_blocks) return 0xFC;

    uint32_t status = 0;
    sd_activity_led_begin();

    if (hdf_raw) {
        if (hdf_read_only) {
            status = 0xFD;
            goto out;
        }
        /* The buffer was filled by ARM CPU stores (Zorro write
         * servicing); XSdPs flushes the range to DDR itself before the
         * DMA. Raw writes only touch data sectors inside the HDF's own
         * extents — no FAT or directory metadata changes — so there is
         * nothing to f_sync and a power loss can't tear the FAT. */
        status = hdf_raw_io(block, num_blocks, buffer, 1);
        goto out;
    }

    Xil_DCacheFlushRange((UINTPTR)buffer, num_blocks * SD_BLOCK_SIZE);

    FRESULT fr = f_lseek(&hdf_file, (FSIZE_t)block * SD_BLOCK_SIZE);
    if (fr != FR_OK) {
        printf("[SD] lseek(%lu) failed: %d\n", (unsigned long)block, (int)fr);
        status = 0xFD;
        goto out;
    }

    UINT n_written = 0;
    fr = f_write(&hdf_file, buffer, num_blocks * SD_BLOCK_SIZE, &n_written);
    if (fr != FR_OK || n_written != num_blocks * SD_BLOCK_SIZE) {
        printf("[SD] write(block=%lu count=%lu) failed: fr=%d n=%u\n",
               (unsigned long)block, (unsigned long)num_blocks,
               (int)fr, n_written);
        status = 0xFD;
        goto out;
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
        status = 0xFD;
        goto out;
    }

out:
    sd_activity_led_end();
    return status;
}

uint32_t sd_storage_capacity(void) {
    return hdf_capacity_blocks;
}

int sd_storage_available(void) {
    return hdf_open;
}
