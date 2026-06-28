/*
 * Minimal host-side FatFs mock for fw_update unit tests.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Declares just the slice of the FatFs API that fw_update.c uses, backed
 * by an in-memory file table (see fatfs_mock.c). FRESULT order matches
 * upstream FatFs so the symbolic comparisons in fw_update.c behave the
 * same on host as on the Zynq.
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

typedef struct { int idx; } FIL;
typedef struct { FSIZE_t fsize; char fname[80]; } FILINFO;

#define FA_READ          0x01u
#define FA_WRITE         0x02u
#define FA_CREATE_ALWAYS 0x08u

FRESULT f_open(FIL *fp, const char *path, unsigned char mode);
FRESULT f_close(FIL *fp);
FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw);
FRESULT f_sync(FIL *fp);
FRESULT f_stat(const char *path, FILINFO *fno);
FRESULT f_rename(const char *path_old, const char *path_new);
FRESULT f_unlink(const char *path);

#endif /* FF_H */
