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
#define FWUP_PATH_MAX    (FWUP_NAME_MAX + 10) /* "0:/ZZ9K/" + 8.3 + NUL */
#define FWUP_MODULE_DIR   "ZZ9K"
#define FWUP_MODULE_PATH  FWUP_VOLUME "/" FWUP_MODULE_DIR
#define FWUP_TEMP_PATH    FWUP_VOLUME "/ZZFWUP.TMP"
#define FWUP_DISCARD_SLOTS 8

static FIL  fwup_file;
static int  fwup_open_flag = 0;
static char fwup_path[FWUP_PATH_MAX];
static uint32_t fwup_bytes_written = 0;
static uint8_t fwup_mirror_seq = 0;

/* The staged job (fw_update_stage). A name is copied with room for a
 * leading '/', FWUP_NAME_MAX characters and one more, so a longer name is
 * still rejected after the copy is terminated. */
#define FWUP_STAGED_NAME (FWUP_NAME_MAX + 3)
static uint8_t  fwup_job_buf[FWUP_MAX_CHUNK] __attribute__((aligned(32)));
static uint16_t fwup_job_cmd;
static uint32_t fwup_job_len;
/* Core 0 only: set when queued, cleared when the job retires or is
 * dropped; the result waits for fw_update_take_result. */
static int      fwup_job_queued;
static int      fwup_job_done;
static uint16_t fwup_job_result;

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
                              const char *target_path,
                              int slot) {
    const char *directory = (strncmp(target_path, FWUP_MODULE_PATH "/",
                                     sizeof(FWUP_MODULE_PATH)) == 0)
                                ? FWUP_MODULE_PATH "/"
                                : FWUP_VOLUME "/";
    if (slot < 0)
        snprintf(discard_path, discard_path_size, "%sZZFWUP.DEL", directory);
    else
        snprintf(discard_path, discard_path_size, "%sZZFWUP%02d.DEL",
                 directory, slot);
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

/* Flat root targets retain their legacy name rule. Modules add exactly one
 * uppercase `ZZ9K/` component and an uppercase 8.3 leaf.  FatFs runs without
 * LFN, so accepting a longer or mixed-case leaf would make the wire contract
 * lie about what reaches the card. */
static int validate_module_leaf(const char *name) {
    size_t base = 0, ext = 0;
    int in_ext = 0;

    if (!name || !*name) return 0;
    for (const char *p = name; *p; p++) {
        const char c = *p;
        if (c == '.') {
            if (in_ext || base == 0) return 0;
            in_ext = 1;
            continue;
        }
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            return 0;
        if (in_ext) {
            if (++ext > 3) return 0;
        } else if (++base > 8) {
            return 0;
        }
    }
    return base && (!in_ext || ext);
}

static int validate_name(const char *raw, char *out_path) {
    if (!raw) return 0;
    const char *p = raw;
    if (*p == '/') p++; /* tolerate a single leading slash */
    if (*p == '\0') return 0;

    if (strncmp(p, FWUP_MODULE_DIR "/", sizeof(FWUP_MODULE_DIR)) == 0) {
        const char *leaf = p + sizeof(FWUP_MODULE_DIR);
        if (!validate_module_leaf(leaf)) return 0;
        snprintf(out_path, FWUP_PATH_MAX, FWUP_MODULE_PATH "/%s", leaf);
        return 1;
    }

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

    if (p[0] == '.' && (p[1] == '\0' || (p[1] == '.' && p[2] == '\0')))
        return 0;
    if (is_reserved_name(p)) return 0;

    snprintf(out_path, FWUP_PATH_MAX, FWUP_VOLUME "/%s", p);
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
    const char *directory = FWUP_VOLUME "/";
    const char *backup_ext = "bak";
    const char *old_ext = "old";
    const char *slash;
    if (name[0] == '0' && name[1] == ':' && name[2] == '/') name += 3;
    slash = strrchr(name, '/');
    if (slash) {
        if (strncmp(name, FWUP_MODULE_DIR "/", sizeof(FWUP_MODULE_DIR)) != 0)
            return 0;
        directory = FWUP_MODULE_PATH "/";
        backup_ext = "BAK";
        old_ext = "OLD";
        name = slash + 1;
    }

    size_t len = strlen(name);
    if (len == 0 || len > FWUP_NAME_MAX) return 0;

    char backup_name[FWUP_NAME_MAX + 1];
    memcpy(backup_name, name, len + 1);

    char *dot = NULL;
    for (char *p = backup_name; *p; p++) {
        if (*p == '.') dot = p;
    }

    const char *ext = (dot && ascii_ieq(dot + 1, "bak")) ? old_ext : backup_ext;
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

    const int written = snprintf(backup_path, backup_path_size, "%s%s",
                                 directory, backup_name);
    return written >= 0 && (size_t)written < backup_path_size &&
           !ascii_ieq(target_path, backup_path);
}

static int reserve_discard_path(char *discard_path, size_t discard_path_size,
                                const char *target_path) {
    char default_path[FWUP_PATH_MAX];
    discard_slot_path(default_path, sizeof(default_path), target_path, -1);
    FILINFO info;
    FRESULT fr = f_stat(default_path, &info);
    if (fr == FR_NO_FILE) {
        strncpy(discard_path, default_path, discard_path_size - 1);
        discard_path[discard_path_size - 1] = '\0';
        return 1;
    }
    if (fr != FR_OK) {
        printf("[FWUP] COMMIT discard path check failed: %d\n", (int)fr);
        return 0;
    }

    for (int slot = 0; slot < FWUP_DISCARD_SLOTS; slot++) {
        discard_slot_path(discard_path, discard_path_size, target_path, slot);
        fr = f_stat(discard_path, &info);
        if (fr == FR_NO_FILE) return 1;
        if (fr != FR_OK) {
            printf("[FWUP] COMMIT discard slot check(%s) failed: %d\n",
                   discard_path, (int)fr);
            return 0;
        }
    }
    printf("[FWUP] COMMIT no free discard path; reboot to clean stale discard files\n");
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
            if (!reserve_discard_path(discard_path, sizeof(discard_path),
                                      fwup_path))
                return FWUP_ERR_CLOSE;
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

static void cleanup_discard_path(const char *target_path, int slot) {
    char discard_path[FWUP_PATH_MAX];
    discard_slot_path(discard_path, sizeof(discard_path), target_path, slot);

    FRESULT fr = f_unlink(discard_path);
    if (fr == FR_OK) {
        printf("[FWUP] cleanup removed %s\n", discard_path);
    } else if (fr != FR_NO_FILE) {
        printf("[FWUP] cleanup unlink(%s) failed: %d\n",
               discard_path, (int)fr);
    }
}

void fw_update_cleanup_backups(void) {
    const char *const cleanup_targets[] = {
        FWUP_VOLUME "/",
        FWUP_MODULE_PATH "/",
    };

    for (size_t target = 0;
         target < sizeof(cleanup_targets) / sizeof(cleanup_targets[0]);
         target++) {
        cleanup_discard_path(cleanup_targets[target], -1);
        for (int slot = 0; slot < FWUP_DISCARD_SLOTS; slot++) {
            cleanup_discard_path(cleanup_targets[target], slot);
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

    if (strncmp(path, FWUP_MODULE_PATH "/", sizeof(FWUP_MODULE_PATH)) == 0) {
        FRESULT mkdir_result = f_mkdir(FWUP_MODULE_PATH);
        if (mkdir_result != FR_OK && mkdir_result != FR_EXIST) {
            printf("[FWUP] OPEN mkdir(%s) failed: %d\n", FWUP_MODULE_PATH,
                   (int)mkdir_result);
            return map_open_error(mkdir_result);
        }
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

uint16_t fw_update_restore(const char *name_buf) {
    if (fwup_open_flag || fwup_path[0] != '\0') {
        printf("[FWUP] RESTORE rejected: a push transfer is in progress\n");
        return FWUP_ERR_STATE;
    }

    char path[FWUP_PATH_MAX];
    if (!validate_name(name_buf, path)) {
        printf("[FWUP] RESTORE: rejected filename\n");
        return FWUP_ERR_BAD_NAME;
    }

    char backup_path[FWUP_PATH_MAX];
    if (!make_backup_path(path, backup_path, sizeof(backup_path))) {
        printf("[FWUP] RESTORE cannot derive backup path for %s\n", path);
        return FWUP_ERR_BAD_NAME;
    }

    /* Refuse before touching the active file when there is no backup to
     * promote — a missing backup must never leave the card without a
     * bootable target. */
    FILINFO info;
    FRESULT fr = f_stat(backup_path, &info);
    if (fr == FR_NO_FILE) {
        printf("[FWUP] RESTORE no backup at %s\n", backup_path);
        return FWUP_ERR_NO_BACKUP;
    }
    if (fr != FR_OK) {
        printf("[FWUP] RESTORE backup check(%s) failed: %d\n",
               backup_path, (int)fr);
        return map_open_error(fr);
    }

    /* Move the currently-active file aside to a discard slot (reclaimed at
     * next boot), then promote the backup onto the target name. Roll the
     * active file back if the promotion fails, so the card is never left
     * without a bootable target. */
    char discard_path[FWUP_PATH_MAX];
    int displaced = 0;
    fr = f_stat(path, &info);
    if (fr == FR_OK) {
        if (!reserve_discard_path(discard_path, sizeof(discard_path), path)) {
            return FWUP_ERR_RESTORE;
        }
        fr = f_rename(path, discard_path);
        if (fr != FR_OK) {
            printf("[FWUP] RESTORE discard active(%s -> %s) failed: %d\n",
                   path, discard_path, (int)fr);
            return FWUP_ERR_RESTORE;
        }
        displaced = 1;
    } else if (fr != FR_NO_FILE) {
        printf("[FWUP] RESTORE active check(%s) failed: %d\n", path, (int)fr);
        return FWUP_ERR_RESTORE;
    }

    fr = f_rename(backup_path, path);
    if (fr != FR_OK) {
        printf("[FWUP] RESTORE promote(%s -> %s) failed: %d\n",
               backup_path, path, (int)fr);
        if (displaced) {
            FRESULT fr_rb = f_rename(discard_path, path);
            if (fr_rb != FR_OK) {
                printf("[FWUP] RESTORE rollback(%s -> %s) failed: %d\n",
                       discard_path, path, (int)fr_rb);
            }
        }
        return FWUP_ERR_RESTORE;
    }

    printf("[FWUP] RESTORE %s <- %s\n", path, backup_path);
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

void fw_update_publish_status(volatile uint8_t *mirror, uint16_t status) {
    uint8_t word[4];
    uint32_t value;

    fwup_mirror_seq++;
    word[0] = (uint8_t)FWUP_STATUS_MIRROR_MAGIC;
    word[1] = fwup_mirror_seq;
    word[2] = (uint8_t)(status >> 8);
    word[3] = (uint8_t)status;
    /* One aligned store: a direct read sees the old or the new word. */
    memcpy(&value, word, sizeof(value));
    *(volatile uint32_t *)mirror = value;
    Xil_DCacheFlushRange((UINTPTR)mirror, 4);
}

uint16_t fw_update_run(uint16_t cmd, const void *buf, uint32_t len) {
    switch (cmd) {
    case FWUP_CMD_OPEN:
        return fw_update_open((const char *)buf);
    case FWUP_CMD_WRITE:
        return fw_update_write(buf, len);
    case FWUP_CMD_CLOSE:
        return fw_update_close();
    case FWUP_CMD_ABORT:
        return fw_update_abort();
    case FWUP_CMD_RESTORE:
        /* Promotes the target's backup over it (see fw_update_restore). */
        return fw_update_restore((const char *)buf);
    default:
        return FWUP_ERR_UNKNOWN;
    }
}

void fw_update_stage(uint16_t cmd, const void *shared, uint32_t len) {
    uint32_t n = 0;

    fwup_job_cmd = cmd;
    fwup_job_len = len;
    if (cmd == FWUP_CMD_OPEN || cmd == FWUP_CMD_RESTORE) {
        n = FWUP_STAGED_NAME;
    } else if (cmd == FWUP_CMD_WRITE && len <= FWUP_MAX_CHUNK) {
        n = len; /* an over-long WRITE is refused by fw_update_write */
    }
    /* The Zorro request loop stored these bytes through core 0's cache,
     * so a plain copy reads them current. */
    if (n != 0) {
        memcpy(fwup_job_buf, shared, n);
    }
    if (cmd == FWUP_CMD_OPEN || cmd == FWUP_CMD_RESTORE) {
        fwup_job_buf[FWUP_STAGED_NAME - 1] = '\0';
    }
    Xil_DCacheFlushRange((UINTPTR)fwup_job_buf, n ? n : 1);
}

uint16_t fw_update_run_staged(void) {
    return fw_update_run(fwup_job_cmd, fwup_job_buf, fwup_job_len);
}

void fw_update_mark_queued(void) {
    fwup_job_queued = 1;
    fwup_job_done = 0;
}

void fw_update_retire(uint16_t result) {
    fwup_job_queued = 0;
    fwup_job_done = 1;
    fwup_job_result = result;
}

void fw_update_retire_failed(void) {
    /* The job faulted on core 1 before returning: report the failure
     * as this command's own error so the client aborts the transfer. */
    switch (fwup_job_cmd) {
    case FWUP_CMD_OPEN:    fw_update_retire(FWUP_ERR_OPEN); break;
    case FWUP_CMD_WRITE:   fw_update_retire(FWUP_ERR_WRITE); break;
    case FWUP_CMD_CLOSE:   fw_update_retire(FWUP_ERR_CLOSE); break;
    case FWUP_CMD_RESTORE: fw_update_retire(FWUP_ERR_RESTORE); break;
    default:               fw_update_retire(FWUP_ERR_UNKNOWN); break;
    }
}

void fw_update_forget_queued(void) {
    fwup_job_queued = 0;
    fwup_job_done = 0;
}

int fw_update_busy(void) {
    return fwup_job_queued;
}

int fw_update_take_result(uint16_t *result) {
    if (!fwup_job_done) {
        return 0;
    }
    fwup_job_done = 0;
    *result = fwup_job_result;
    return 1;
}
