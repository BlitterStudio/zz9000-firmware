/*
 * Minimal host-side FatFs mock for the audio config tests.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Declares the slice of the FatFs API that zz_config.c uses for both
 * the loader (read) and the U5 atomic save (write + rename), backed by
 * an in-memory single-volume filesystem with per-call failure
 * injection (see fatfs_mock.c). FRESULT order matches upstream FatFs.
 */
#ifndef FF_H
#define FF_H

#include <stddef.h>

typedef unsigned int  UINT;
typedef unsigned long FSIZE_t;

typedef enum {
    FR_OK = 0,
    FR_DISK_ERR,
    FR_INT_ERR,
    FR_NOT_READY,
    FR_NO_FILE,
    FR_NO_PATH,
    FR_INVALID_NAME,
    FR_DENIED,
    FR_EXIST,
    FR_INVALID_OBJECT,
    FR_WRITE_PROTECTED,
    FR_INVALID_DRIVE,
    FR_NOT_ENABLED,
    FR_NO_FILESYSTEM
} FRESULT;

typedef struct { int dummy; } FATFS;
typedef struct { int file; size_t pos; } FIL;

#define FA_READ         0x01u
#define FA_WRITE        0x02u
#define FA_CREATE_ALWAYS 0x08u

FRESULT f_mount(FATFS *fs, const char *path, unsigned char opt);
FRESULT f_open(FIL *fp, const char *path, unsigned char mode);
FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw);
FRESULT f_sync(FIL *fp);
FRESULT f_close(FIL *fp);
FRESULT f_unlink(const char *path);
FRESULT f_rename(const char *path_old, const char *path_new);

/* test hooks */
void mock_fs_reset(void);
void mock_fs_set_file(const char *path, const char *contents);
const char *mock_fs_file(const char *path); /* NULL when absent */
int mock_fs_file_len(const char *path);     /* -1 when absent */
void mock_fail_write(int nth);              /* f_write fails on nth call */
void mock_fail_rename(int nth);             /* f_rename fails on nth call */

#endif /* FF_H */
