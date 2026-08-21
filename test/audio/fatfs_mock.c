/*
 * In-memory FatFs mock for the audio config tests.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One flat volume of named files (paths like "0:/ZZ9000.CFG"). Writes
 * append to the opened file, f_rename moves file contents between
 * paths, and either API can be made to fail on its nth call so the
 * save path's temp-then-replace discipline is observable from the
 * host suite.
 */
#include <string.h>

#include "ff.h"

#define MOCK_FILES_MAX 8
#define MOCK_FILE_CAP  8192
#define MOCK_PATH_MAX  64

struct mock_file {
    int used;
    char path[MOCK_PATH_MAX];
    size_t len;
    char data[MOCK_FILE_CAP];
};

static struct mock_file files[MOCK_FILES_MAX];
static int write_calls;
static int rename_calls;
static int fail_write_at;
static int fail_rename_at;

static struct mock_file *find_file(const char *path)
{
    int i;
    for (i = 0; i < MOCK_FILES_MAX; i++)
        if (files[i].used && strcmp(files[i].path, path) == 0)
            return &files[i];
    return NULL;
}

static struct mock_file *create_file(const char *path)
{
    struct mock_file *f = find_file(path);
    if (f != NULL)
        return f;
    if (strlen(path) >= MOCK_PATH_MAX)
        return NULL;
    for (int i = 0; i < MOCK_FILES_MAX; i++) {
        if (!files[i].used) {
            files[i].used = 1;
            strcpy(files[i].path, path);
            files[i].len = 0;
            files[i].data[0] = '\0';
            return &files[i];
        }
    }
    return NULL;
}

void mock_fs_reset(void)
{
    memset(files, 0, sizeof(files));
    write_calls = 0;
    rename_calls = 0;
    fail_write_at = 0;
    fail_rename_at = 0;
}

void mock_fs_set_file(const char *path, const char *contents)
{
    struct mock_file *f = create_file(path);
    size_t n = strlen(contents);

    if (f == NULL || n >= MOCK_FILE_CAP)
        return;
    memcpy(f->data, contents, n + 1);
    f->len = n;
}

const char *mock_fs_file(const char *path)
{
    struct mock_file *f = find_file(path);
    return f != NULL ? f->data : NULL;
}

int mock_fs_file_len(const char *path)
{
    struct mock_file *f = find_file(path);
    return f != NULL ? (int)f->len : -1;
}

void mock_fail_write(int nth)   { fail_write_at = nth; }
void mock_fail_rename(int nth)  { fail_rename_at = nth; }

/* ---- FatFs API surface used by zz_config.c ---- */

FRESULT f_mount(FATFS *fs, const char *path, unsigned char opt)
{
    (void)fs;
    (void)path;
    (void)opt;
    return FR_OK;
}

FRESULT f_open(FIL *fp, const char *path, unsigned char mode)
{
    struct mock_file *f;

    if (mode & FA_WRITE) {
        /* The writer only ever opens FA_CREATE_ALWAYS | FA_WRITE. */
        f = create_file(path);
        if (f == NULL)
            return FR_DENIED;
        f->len = 0;
        f->data[0] = '\0';
    } else {
        f = find_file(path);
        if (f == NULL)
            return FR_NO_FILE;
    }
    fp->file = (int)(f - files);
    fp->pos = 0;
    return FR_OK;
}

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br)
{
    struct mock_file *f = &files[fp->file];
    size_t n = f->len - fp->pos;

    if (n > btr)
        n = btr;
    memcpy(buff, f->data + fp->pos, n);
    fp->pos += n;
    *br = (UINT)n;
    return FR_OK;
}

FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw)
{
    struct mock_file *f;

    write_calls++;
    if (fail_write_at != 0 && write_calls == fail_write_at) {
        *bw = 0;
        return FR_DISK_ERR;
    }

    f = &files[fp->file];
    if (fp->pos + btw > MOCK_FILE_CAP)
        return FR_DENIED;
    memcpy(f->data + fp->pos, buff, btw);
    fp->pos += btw;
    if (fp->pos > f->len)
        f->len = fp->pos;
    f->data[f->len] = '\0';
    *bw = btw;
    return FR_OK;
}

FRESULT f_sync(FIL *fp) { (void)fp; return FR_OK; }
FRESULT f_close(FIL *fp) { (void)fp; return FR_OK; }

FRESULT f_unlink(const char *path)
{
    struct mock_file *f = find_file(path);

    if (f == NULL)
        return FR_NO_FILE;
    f->used = 0;
    return FR_OK;
}

FRESULT f_rename(const char *path_old, const char *path_new)
{
    struct mock_file *src = find_file(path_old);
    struct mock_file *dst;

    rename_calls++;
    if (fail_rename_at != 0 && rename_calls == fail_rename_at)
        return FR_DISK_ERR;
    if (src == NULL)
        return FR_NO_FILE;

    dst = create_file(path_new);
    if (dst == NULL || dst->len + src->len >= MOCK_FILE_CAP)
        return FR_DENIED;
    memcpy(dst->data, src->data, src->len + 1);
    dst->len = src->len;
    src->used = 0;
    return FR_OK;
}
