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
#define FWUP_PATH_MAX    (FWUP_NAME_MAX + 4) /* "0:/" + name + NUL */
#define FWUP_TEMP_PATH   FWUP_VOLUME "/ZZFWUP.TMP"
#define FWUP_DISCARD_PATH FWUP_VOLUME "/ZZFWUP.DEL"
#define FWUP_DISCARD_SLOTS 8

static FIL  fwup_file;
static int  fwup_open_flag = 0;
static char fwup_path[FWUP_PATH_MAX];
static uint32_t fwup_bytes_written = 0;

static int ascii_upper(int c) {
    return (c >= 'a' && c <= 'z') ? (c - ('a' - 'A')) : c;
}

static int ascii_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (ascii_upper((unsigned char)*a) != ascii_upper((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int ascii_iprefix(const char *s, const char *prefix) {
    while (*prefix) {
        if (ascii_upper((unsigned char)*s) !=
            ascii_upper((unsigned char)*prefix)) {
            return 0;
        }
        s++;
        prefix++;
    }
    return 1;
}

static int is_reserved_name(const char *name) {
    return ascii_iprefix(name, "ZZFWUP");
}

static void discard_slot_path(char *discard_path,
                              size_t discard_path_size,
                              int slot) {
    snprintf(discard_path, discard_path_size, FWUP_VOLUME "/ZZFWUP%02d.DEL",
             slot);
}

static uint16_t map_open_error(FRESULT fr) {
    return (fr == FR_NOT_READY || fr == FR_NO_FILESYSTEM || fr == FR_NOT_ENABLED)
               ? FWUP_ERR_NO_SD
               : FWUP_ERR_OPEN;
}

static uint16_t map_path_error(FRESULT fr) {
    if (fr == FR_INVALID_NAME || fr == FR_INVALID_DRIVE) {
        return FWUP_ERR_BAD_NAME;
    }
    return map_open_error(fr);
}

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
    if (is_reserved_name(p)) {
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
}

static int make_backup_path(const char *target_path,
                            char *backup_path,
                            size_t backup_path_size) {
    const char *name = target_path;
    if (name[0] == '0' && name[1] == ':' && name[2] == '/') {
        name += 3;
    }

    size_t len = strlen(name);
    if (len == 0 || len > FWUP_NAME_MAX || backup_path_size < FWUP_PATH_MAX) {
        return 0;
    }

    char backup_name[FWUP_NAME_MAX + 1];
    memcpy(backup_name, name, len + 1);

    char *dot = NULL;
    for (char *p = backup_name; *p; p++) {
        if (*p == '.') dot = p;
    }

    const char *ext = (dot && ascii_ieq(dot + 1, "bak")) ? "old" : "bak";
    size_t base_len = dot ? (size_t)(dot - backup_name) : len;
    if (dot && base_len + 4 <= FWUP_NAME_MAX) {
        backup_name[base_len] = '.';
        backup_name[base_len + 1] = ext[0];
        backup_name[base_len + 2] = ext[1];
        backup_name[base_len + 3] = ext[2];
        backup_name[base_len + 4] = '\0';
    } else if (!dot && len + 4 <= FWUP_NAME_MAX) {
        backup_name[len] = '.';
        backup_name[len + 1] = ext[0];
        backup_name[len + 2] = ext[1];
        backup_name[len + 3] = ext[2];
        backup_name[len + 4] = '\0';
    } else {
        backup_name[FWUP_NAME_MAX - 4] = '.';
        backup_name[FWUP_NAME_MAX - 3] = ext[0];
        backup_name[FWUP_NAME_MAX - 2] = ext[1];
        backup_name[FWUP_NAME_MAX - 1] = ext[2];
        backup_name[FWUP_NAME_MAX] = '\0';
    }

    snprintf(backup_path, backup_path_size, FWUP_VOLUME "/%s", backup_name);
    return !ascii_ieq(target_path, backup_path);
}

static int reserve_discard_path(char *discard_path, size_t discard_path_size) {
    FILINFO info;
    FRESULT fr = f_stat(FWUP_DISCARD_PATH, &info);
    if (fr == FR_NO_FILE) {
        strncpy(discard_path, FWUP_DISCARD_PATH, discard_path_size - 1);
        discard_path[discard_path_size - 1] = '\0';
        return 1;
    }
    if (fr != FR_OK) {
        printf("[FWUP] COMMIT discard path check failed: %d\n", (int)fr);
        return 0;
    }

    for (int slot = 0; slot < FWUP_DISCARD_SLOTS; slot++) {
        discard_slot_path(discard_path, discard_path_size, slot);
        fr = f_stat(discard_path, &info);
        if (fr == FR_NO_FILE) {
            return 1;
        }
        if (fr != FR_OK) {
            printf("[FWUP] COMMIT discard slot check(%s) failed: %d\n",
                   discard_path, (int)fr);
            return 0;
        }
    }

    printf("[FWUP] COMMIT no free discard path; reboot to clean ZZFWUP*.DEL\n");
    return 0;
}

static uint16_t commit_temp_file(void) {
    char backup_path[FWUP_PATH_MAX];
    if (!make_backup_path(fwup_path, backup_path, sizeof(backup_path))) {
        printf("[FWUP] COMMIT cannot derive backup path for %s\n", fwup_path);
        return FWUP_ERR_CLOSE;
    }

    FILINFO info;
    int have_backup = 0;
    int displaced_backup = 0;
    char discard_path[FWUP_PATH_MAX];
    discard_path[0] = '\0';

    FRESULT fr = f_stat(fwup_path, &info);
    if (fr == FR_OK) {
        fr = f_stat(backup_path, &info);
        if (fr == FR_OK) {
            if (!reserve_discard_path(discard_path, sizeof(discard_path))) {
                return FWUP_ERR_CLOSE;
            }
            fr = f_rename(backup_path, discard_path);
            if (fr != FR_OK) {
                printf("[FWUP] COMMIT displace backup(%s -> %s) failed: %d\n",
                       backup_path, discard_path, (int)fr);
                return FWUP_ERR_CLOSE;
            }
            displaced_backup = 1;
        } else if (fr != FR_NO_FILE) {
            printf("[FWUP] COMMIT backup check(%s) failed: %d\n",
                   backup_path, (int)fr);
            return FWUP_ERR_CLOSE;
        }

        fr = f_rename(fwup_path, backup_path);
        if (fr == FR_OK) {
            have_backup = 1;
            printf("[FWUP] COMMIT saved previous %s as %s\n",
                   fwup_path, backup_path);
        } else {
            printf("[FWUP] COMMIT backup(%s -> %s) failed: %d\n",
                   fwup_path, backup_path, (int)fr);
            if (displaced_backup) {
                f_rename(discard_path, backup_path);
            }
            return FWUP_ERR_CLOSE;
        }
    } else if (fr != FR_NO_FILE) {
        printf("[FWUP] COMMIT target check(%s) failed: %d\n",
               fwup_path, (int)fr);
        return FWUP_ERR_CLOSE;
    }

    fr = f_rename(FWUP_TEMP_PATH, fwup_path);
    if (fr != FR_OK) {
        printf("[FWUP] COMMIT rename(%s -> %s) failed: %d\n",
               FWUP_TEMP_PATH, fwup_path, (int)fr);
        if (have_backup) {
            FRESULT fr_restore = f_rename(backup_path, fwup_path);
            if (fr_restore != FR_OK) {
                printf("[FWUP] COMMIT restore(%s) failed: %d\n",
                       fwup_path, (int)fr_restore);
            }
        }
        if (displaced_backup) {
            FRESULT fr_restore = f_rename(discard_path, backup_path);
            if (fr_restore != FR_OK) {
                printf("[FWUP] COMMIT restore backup(%s) failed: %d\n",
                       backup_path, (int)fr_restore);
            }
        }
        return FWUP_ERR_CLOSE;
    }

    if (have_backup) {
        printf("[FWUP] COMMIT kept previous file at %s\n", backup_path);
    }
    if (displaced_backup) {
        printf("[FWUP] COMMIT old backup queued for cleanup at %s\n",
               discard_path);
    }
    return FWUP_OK;
}

void fw_update_cleanup_backups(void) {
    FRESULT fr = f_unlink(FWUP_DISCARD_PATH);
    if (fr == FR_OK) {
        printf("[FWUP] cleanup removed %s\n", FWUP_DISCARD_PATH);
    } else if (fr != FR_NO_FILE) {
        printf("[FWUP] cleanup unlink(%s) failed: %d\n",
               FWUP_DISCARD_PATH, (int)fr);
    }

    for (int slot = 0; slot < FWUP_DISCARD_SLOTS; slot++) {
        char discard_path[FWUP_PATH_MAX];
        discard_slot_path(discard_path, sizeof(discard_path), slot);
        fr = f_unlink(discard_path);
        if (fr == FR_OK) {
            printf("[FWUP] cleanup removed %s\n", discard_path);
        } else if (fr != FR_NO_FILE) {
            printf("[FWUP] cleanup unlink(%s) failed: %d\n",
                   discard_path, (int)fr);
        }
    }

}

uint16_t fw_update_open(const char *name_buf) {
    if (fwup_open_flag || fwup_path[0] != '\0') {
        /* Caller forgot to CLOSE/ABORT the previous transfer. Discard
         * it rather than leak the FIL/temp file — the new OPEN wins. */
        printf("[FWUP] OPEN with prior file still open (%s); discarding\n", fwup_path);
        close_silent();
        f_unlink(FWUP_TEMP_PATH);
        fwup_path[0] = '\0';
    }

    char path[FWUP_PATH_MAX];
    if (!validate_name(name_buf, path)) {
        printf("[FWUP] OPEN: rejected filename\n");
        return FWUP_ERR_BAD_NAME;
    }

    FILINFO info;
    FRESULT fr = f_stat(path, &info);
    if (fr != FR_OK && fr != FR_NO_FILE) {
        printf("[FWUP] OPEN path check(%s) failed: %d\n", path, (int)fr);
        return map_path_error(fr);
    }

    fr = f_unlink(FWUP_TEMP_PATH);
    if (fr != FR_OK && fr != FR_NO_FILE) {
        printf("[FWUP] OPEN unlink stale temp failed: %d\n", (int)fr);
        return map_open_error(fr);
    }

    fr = f_open(&fwup_file, FWUP_TEMP_PATH, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) {
        printf("[FWUP] OPEN(%s for %s) failed: %d\n", FWUP_TEMP_PATH, path, (int)fr);
        return map_open_error(fr);
    }

    fwup_open_flag = 1;
    fwup_bytes_written = 0;
    strncpy(fwup_path, path, sizeof(fwup_path) - 1);
    fwup_path[sizeof(fwup_path) - 1] = '\0';
    printf("[FWUP] OPEN %s via %s\n", fwup_path, FWUP_TEMP_PATH);
    return FWUP_OK;
}

uint16_t fw_update_write(const void *buf, uint32_t len) {
    if (!fwup_open_flag) return FWUP_ERR_STATE;
    if (len == 0 || len > FWUP_MAX_CHUNK) return FWUP_ERR_LEN;

    /* Zorro writes are staged by the ARM request loop as CPU stores into
     * cacheable DDR. Clean the range before f_write so any direct SD DMA
     * path inside FatFs sees the fresh bytes in memory. */
    Xil_DCacheFlushRange((UINTPTR)buf, len);

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
    uint16_t commit_status = commit_temp_file();
    if (commit_status != FWUP_OK) {
        return commit_status;
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
        FRESULT fr = f_unlink(FWUP_TEMP_PATH);
        if (fr != FR_OK && fr != FR_NO_FILE) {
            printf("[FWUP] ABORT unlink(%s) failed: %d\n", FWUP_TEMP_PATH, (int)fr);
        } else {
            printf("[FWUP] ABORT %s\n", saved_path);
        }
    }
    fwup_path[0] = '\0';
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
