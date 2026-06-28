/*
 * In-memory FatFs mock for fw_update unit tests.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Flat namespace, exact-string paths (fw_update.c always builds canonical
 * "0:/NAME" paths, so case folding is unnecessary here). Just enough of
 * f_open/f_write/f_sync/f_close/f_stat/f_rename/f_unlink to drive the
 * firmware-file commit and restore paths.
 */
#include <string.h>
#include "fatfs_mock.h"

#define MOCK_MAX_FILES 32
#define MOCK_PATH_MAX  80
#define MOCK_TAG_MAX   16

struct mock_file {
    int  used;
    int  open;
    char path[MOCK_PATH_MAX];
    char tag[MOCK_TAG_MAX];
};

static struct mock_file g_files[MOCK_MAX_FILES];
static int g_rename_calls = 0;
static int g_rename_fail_at = 0;

static int find_file(const char *path) {
    for (int i = 0; i < MOCK_MAX_FILES; i++) {
        if (g_files[i].used && strcmp(g_files[i].path, path) == 0) {
            return i;
        }
    }
    return -1;
}

static int alloc_file(const char *path, const char *tag) {
    for (int i = 0; i < MOCK_MAX_FILES; i++) {
        if (!g_files[i].used) {
            g_files[i].used = 1;
            g_files[i].open = 0;
            strncpy(g_files[i].path, path, MOCK_PATH_MAX - 1);
            g_files[i].path[MOCK_PATH_MAX - 1] = '\0';
            strncpy(g_files[i].tag, tag ? tag : "", MOCK_TAG_MAX - 1);
            g_files[i].tag[MOCK_TAG_MAX - 1] = '\0';
            return i;
        }
    }
    return -1;
}

void mock_reset(void) {
    memset(g_files, 0, sizeof(g_files));
    g_rename_calls = 0;
    g_rename_fail_at = 0;
}

int mock_add_file(const char *path, const char *tag) {
    if (find_file(path) >= 0) return 0;
    return alloc_file(path, tag) >= 0;
}

int mock_exists(const char *path) {
    return find_file(path) >= 0;
}

const char *mock_tag(const char *path) {
    int i = find_file(path);
    return (i >= 0) ? g_files[i].tag : "";
}

void mock_fail_rename_at(int nth) {
    g_rename_fail_at = nth;
}

/* ---- FatFs API surface used by fw_update.c ---- */

FRESULT f_open(FIL *fp, const char *path, unsigned char mode) {
    (void)mode; /* tests only ever create-always|write the temp file */
    int i = find_file(path);
    if (i < 0) {
        i = alloc_file(path, "");
        if (i < 0) return FR_DISK_ERR;
    } else {
        g_files[i].tag[0] = '\0'; /* CREATE_ALWAYS truncates */
    }
    g_files[i].open = 1;
    fp->idx = i;
    return FR_OK;
}

FRESULT f_close(FIL *fp) {
    if (fp->idx >= 0 && fp->idx < MOCK_MAX_FILES) {
        g_files[fp->idx].open = 0;
    }
    return FR_OK;
}

FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw) {
    (void)buff;
    if (fp->idx < 0 || fp->idx >= MOCK_MAX_FILES || !g_files[fp->idx].used) {
        return FR_INVALID_OBJECT;
    }
    if (bw) *bw = btw;
    return FR_OK;
}

FRESULT f_sync(FIL *fp) {
    (void)fp;
    return FR_OK;
}

FRESULT f_stat(const char *path, FILINFO *fno) {
    int i = find_file(path);
    if (i < 0) return FR_NO_FILE;
    if (fno) {
        memset(fno, 0, sizeof(*fno));
        strncpy(fno->fname, path, sizeof(fno->fname) - 1);
    }
    return FR_OK;
}

FRESULT f_rename(const char *path_old, const char *path_new) {
    g_rename_calls++;
    if (g_rename_fail_at && g_rename_calls == g_rename_fail_at) {
        return FR_DISK_ERR;
    }
    int i = find_file(path_old);
    if (i < 0) return FR_NO_FILE;
    if (find_file(path_new) >= 0) return FR_EXIST;
    strncpy(g_files[i].path, path_new, MOCK_PATH_MAX - 1);
    g_files[i].path[MOCK_PATH_MAX - 1] = '\0';
    return FR_OK;
}

FRESULT f_unlink(const char *path) {
    int i = find_file(path);
    if (i < 0) return FR_NO_FILE;
    g_files[i].used = 0;
    return FR_OK;
}
