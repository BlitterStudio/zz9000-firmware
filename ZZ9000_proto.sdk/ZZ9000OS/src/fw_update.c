/*
 * ZZ9000 Firmware-File Push
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <ff.h>
#include "xil_cache.h"
#include "fw_update.h"

/* Matches the visible Zorro window at cardbase+0xA000..0x10000 used by
 * the SD/USB block paths. WRITE chunks must fit inside it. */
#define FWUP_MAX_CHUNK   24576u
#define FWUP_VOLUME      "0:"
#define FWUP_NAME_MAX    64

static FIL  fwup_file;
static int  fwup_open_flag = 0;
static char fwup_path[FWUP_NAME_MAX + 4]; /* "0:/" + name + NUL */
static uint32_t fwup_bytes_written = 0;

/* Allow `[A-Za-z0-9._-]` plus a single optional leading '/'. Rejects
 * '..', backslash, ':' (FatFs volume prefix), control chars, and any
 * other shenanigan that could escape the FAT root. We deliberately do
 * not support subdirectories — flat root only — to keep the validator
 * tight and the failure modes obvious. */
static int validate_name(const char *raw, char *out_path) {
    if (!raw) return 0;
    const char *p = raw;
    if (*p == '/') p++; /* tolerate a single leading slash */
    if (*p == '\0') return 0;

    size_t n = 0;
    for (const char *q = p; *q; q++) {
        char c = *q;
        if (c >= 'A' && c <= 'Z') goto ok;
        if (c >= 'a' && c <= 'z') goto ok;
        if (c >= '0' && c <= '9') goto ok;
        if (c == '.' || c == '_' || c == '-') goto ok;
        return 0;
    ok:
        n++;
        if (n > FWUP_NAME_MAX) return 0;
    }
    if (n == 0) return 0;

    /* Reject ".." or a leading "." that would resolve to current dir. */
    if (p[0] == '.' && (p[1] == '\0' || (p[1] == '.' && p[2] == '\0'))) {
        return 0;
    }

    /* "0:/" + filename + NUL. */
    out_path[0] = '0';
    out_path[1] = ':';
    out_path[2] = '/';
    memcpy(out_path + 3, p, n);
    out_path[3 + n] = '\0';
    return 1;
}

static void close_silent(void) {
    if (fwup_open_flag) {
        f_close(&fwup_file);
        fwup_open_flag = 0;
    }
    fwup_bytes_written = 0;
    fwup_path[0] = '\0';
}

uint16_t fw_update_open(const char *name_buf) {
    if (fwup_open_flag) {
        /* Caller forgot to CLOSE/ABORT the previous transfer. Discard
         * it rather than leak the FIL — the new OPEN wins. */
        printf("[FWUP] OPEN with prior file still open (%s); discarding\n", fwup_path);
        close_silent();
    }

    char path[FWUP_NAME_MAX + 4];
    if (!validate_name(name_buf, path)) {
        printf("[FWUP] OPEN: rejected filename\n");
        return FWUP_ERR_BAD_NAME;
    }

    FRESULT fr = f_open(&fwup_file, path, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) {
        printf("[FWUP] OPEN(%s) failed: %d\n", path, (int)fr);
        return (fr == FR_NOT_READY || fr == FR_NO_FILESYSTEM) ? FWUP_ERR_NO_SD
                                                              : FWUP_ERR_OPEN;
    }

    fwup_open_flag = 1;
    fwup_bytes_written = 0;
    strncpy(fwup_path, path, sizeof(fwup_path) - 1);
    fwup_path[sizeof(fwup_path) - 1] = '\0';
    printf("[FWUP] OPEN %s\n", fwup_path);
    return FWUP_OK;
}

uint16_t fw_update_write(const void *buf, uint32_t len) {
    if (!fwup_open_flag) return FWUP_ERR_STATE;
    if (len == 0 || len > FWUP_MAX_CHUNK) return FWUP_ERR_LEN;

    /* The Amiga staged this chunk via Zorro byte writes (AXI_HP into
     * DDR, non-coherent with the ARM D-cache). Invalidate the range so
     * f_write reads the fresh bytes rather than stale cache lines. */
    Xil_DCacheInvalidateRange((UINTPTR)buf, len);

    UINT n_written = 0;
    FRESULT fr = f_write(&fwup_file, buf, len, &n_written);
    if (fr != FR_OK || n_written != len) {
        printf("[FWUP] WRITE(%lu) failed: fr=%d n=%u\n",
               (unsigned long)len, (int)fr, n_written);
        /* Don't auto-close — the Amiga can decide to ABORT (deleting
         * the partial) or CLOSE (keeping what it has, e.g. for debug). */
        return FWUP_ERR_WRITE;
    }

    fwup_bytes_written += len;
    return FWUP_OK;
}

uint16_t fw_update_close(void) {
    if (!fwup_open_flag) return FWUP_ERR_STATE;

    /* Always run f_close, even when f_sync fails — skipping it would
     * leak the FIL inside FatFs's volume tracking. Surface whichever
     * error came first. */
    FRESULT fr_sync = f_sync(&fwup_file);
    FRESULT fr_close = f_close(&fwup_file);
    fwup_open_flag = 0;
    FRESULT fr = (fr_sync != FR_OK) ? fr_sync : fr_close;
    if (fr != FR_OK) {
        printf("[FWUP] CLOSE(%s) failed: sync=%d close=%d\n",
               fwup_path, (int)fr_sync, (int)fr_close);
        return FWUP_ERR_CLOSE;
    }
    printf("[FWUP] CLOSE %s (%lu bytes)\n", fwup_path,
           (unsigned long)fwup_bytes_written);
    fwup_bytes_written = 0;
    fwup_path[0] = '\0';
    return FWUP_OK;
}

uint16_t fw_update_abort(void) {
    if (!fwup_open_flag && fwup_path[0] == '\0') {
        /* Nothing in flight — treat as a no-op success so the caller
         * can use ABORT as a "make sure we're idle" reset. */
        return FWUP_OK;
    }
    char saved_path[sizeof(fwup_path)];
    strncpy(saved_path, fwup_path, sizeof(saved_path));
    saved_path[sizeof(saved_path) - 1] = '\0';

    close_silent();

    if (saved_path[0] != '\0') {
        FRESULT fr = f_unlink(saved_path);
        if (fr != FR_OK && fr != FR_NO_FILE) {
            printf("[FWUP] ABORT unlink(%s) failed: %d\n", saved_path, (int)fr);
        } else {
            printf("[FWUP] ABORT %s\n", saved_path);
        }
    }
    return FWUP_OK;
}

void fw_update_reset(void) {
    /* On Amiga reset the partial file is junk — drop it. f_unlink
     * failure here is non-fatal; the file remains until the user
     * cleans up manually. */
    if (fwup_open_flag || fwup_path[0] != '\0') {
        fw_update_abort();
    }
}
