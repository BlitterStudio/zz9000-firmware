/*
 * Minimal host-side FatFs mock for the zz_config unit tests.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Declares just the slice of the FatFs API that zz_config.c uses,
 * backed by a single injectable in-memory file (see config_test.c).
 * FRESULT order matches upstream FatFs.
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
typedef struct { size_t pos; } FIL;

#define FA_READ 0x01u

FRESULT f_mount(FATFS *fs, const char *path, unsigned char opt);
FRESULT f_open(FIL *fp, const char *path, unsigned char mode);
FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT f_close(FIL *fp);

/* test hooks */
void mock_set_file(const char *contents);   /* NULL = no ZZ9000.CFG */
void mock_set_mount_result(FRESULT fr);
int  mock_mount_balance(void);              /* mounts minus unmounts */

#endif /* FF_H */
